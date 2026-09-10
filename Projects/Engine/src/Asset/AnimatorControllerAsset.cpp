/// @file    AnimatorControllerAsset.cpp
/// @brief   .animcontroller の TOML 入出力と AnimatorComponent への適用。
/// @author  Hasegawa Jin
/// @date    2026-06-13
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/GuidRefCodec.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <toml++/toml.hpp>
#include <sstream>
#include <utility>

namespace fbzz::asset {
namespace {

toml::table WriteMotion(const scene::BlendTreeMotion& motion)
{
    toml::table table;
    table.insert("threshold", static_cast<double>(motion.threshold));
    table.insert("posX", static_cast<double>(motion.posX));
    table.insert("posY", static_cast<double>(motion.posY));
    table.insert("sourcePath", motion.sourcePath);
    table.insert("clipName", motion.clipName);
    table.insert("clipIndex", static_cast<int64_t>(motion.clipIndex));
    table.insert("speed", static_cast<double>(motion.speed));
    table.insert("ikWeight", static_cast<double>(motion.ikWeight));
    return table;
}

toml::table WriteTransition(const scene::AnimationTransition& transition)
{
    toml::table table;
    table.insert("toStateName", transition.toStateName);
    table.insert("hasExitTime", transition.hasExitTime);
    table.insert("exitTime", static_cast<double>(transition.exitTime));
    table.insert("fixedDuration", transition.fixedDuration);
    table.insert("transitionDuration", static_cast<double>(transition.transitionDuration));
    toml::array conditions;
    for (const auto& condition : transition.conditions) {
        toml::table conditionTable;
        conditionTable.insert("paramName", condition.paramName);
        conditionTable.insert("op", static_cast<int64_t>(condition.op));
        conditionTable.insert("threshold", static_cast<double>(condition.threshold));
        conditions.push_back(std::move(conditionTable));
    }
    table.insert("conditions", std::move(conditions));
    return table;
}

scene::BlendTreeMotion ReadMotion(const toml::table& table)
{
    scene::BlendTreeMotion motion;
    motion.threshold = static_cast<float>(table["threshold"].value_or(0.0));
    motion.posX = static_cast<float>(table["posX"].value_or(0.0));
    motion.posY = static_cast<float>(table["posY"].value_or(0.0));
    motion.sourcePath = table["sourcePath"].value_or(std::string{});
    motion.clipName = table["clipName"].value_or(std::string{});
    motion.clipIndex = static_cast<int>(table["clipIndex"].value_or(int64_t{-1}));
    motion.speed = static_cast<float>(table["speed"].value_or(1.0));
    motion.ikWeight = static_cast<float>(table["ikWeight"].value_or(1.0));
    return motion;
}

scene::AnimationTransition ReadTransition(const toml::table& table)
{
    scene::AnimationTransition transition;
    transition.toStateName = table["toStateName"].value_or(std::string{});
    transition.hasExitTime = table["hasExitTime"].value_or(false);
    transition.exitTime = static_cast<float>(table["exitTime"].value_or(1.0));
    transition.fixedDuration = table["fixedDuration"].value_or(true);
    transition.transitionDuration =
        static_cast<float>(table["transitionDuration"].value_or(0.25));
    if (const auto* conditions = table["conditions"].as_array()) {
        for (const auto& element : *conditions) {
            const auto* conditionTable = element.as_table();
            if (!conditionTable) continue;
            scene::AnimatorCondition condition;
            condition.paramName = (*conditionTable)["paramName"].value_or(std::string{});
            condition.op = static_cast<scene::ConditionOp>(
                (*conditionTable)["op"].value_or(int64_t{4}));
            condition.threshold =
                static_cast<float>((*conditionTable)["threshold"].value_or(0.0));
            transition.conditions.push_back(std::move(condition));
        }
    }
    return transition;
}

toml::table WriteEditorLayout(const AnimatorGraphLayout& layout)
{
    toml::table table;
    table.insert("entryX", static_cast<double>(layout.entryPosition.x));
    table.insert("entryY", static_cast<double>(layout.entryPosition.y));
    table.insert("anyStateX", static_cast<double>(layout.anyStatePosition.x));
    table.insert("anyStateY", static_cast<double>(layout.anyStatePosition.y));
    table.insert("slotX", static_cast<double>(layout.slotPosition.x));
    table.insert("slotY", static_cast<double>(layout.slotPosition.y));

    toml::array nodes;
    for (const auto& [stateName, pos] : layout.nodePositions) {
        toml::table node;
        node.insert("stateName", stateName);
        node.insert("x", static_cast<double>(pos.x));
        node.insert("y", static_cast<double>(pos.y));
        nodes.push_back(std::move(node));
    }
    table.insert("nodes", std::move(nodes));

    toml::array blendTreeMotions;
    for (const auto& [stateName, positions] : layout.blendTreeMotionPositions) {
        for (size_t motionIndex = 0; motionIndex < positions.size(); ++motionIndex) {
            toml::table motion;
            motion.insert("stateName", stateName);
            motion.insert("motionIndex", static_cast<int64_t>(motionIndex));
            motion.insert("x", static_cast<double>(positions[motionIndex].x));
            motion.insert("y", static_cast<double>(positions[motionIndex].y));
            blendTreeMotions.push_back(std::move(motion));
        }
    }
    table.insert("blendTreeMotions", std::move(blendTreeMotions));
    return table;
}

AnimatorGraphLayout ReadEditorLayout(const toml::table& table)
{
    AnimatorGraphLayout layout;
    layout.entryPosition = {
        static_cast<float>(table["entryX"].value_or(-220.0)),
        static_cast<float>(table["entryY"].value_or(80.0))
    };
    layout.anyStatePosition = {
        static_cast<float>(table["anyStateX"].value_or(-220.0)),
        static_cast<float>(table["anyStateY"].value_or(260.0))
    };
    layout.slotPosition = {
        static_cast<float>(table["slotX"].value_or(-220.0)),
        static_cast<float>(table["slotY"].value_or(440.0))
    };

    if (const auto* nodes = table["nodes"].as_array()) {
        for (const auto& element : *nodes) {
            const auto* node = element.as_table();
            if (!node) continue;
            const std::string stateName = (*node)["stateName"].value_or(std::string{});
            if (stateName.empty()) continue;
            layout.nodePositions[stateName] = {
                static_cast<float>((*node)["x"].value_or(0.0)),
                static_cast<float>((*node)["y"].value_or(0.0))
            };
        }
    }

    if (const auto* motions = table["blendTreeMotions"].as_array()) {
        for (const auto& element : *motions) {
            const auto* motion = element.as_table();
            if (!motion) continue;
            const std::string stateName = (*motion)["stateName"].value_or(std::string{});
            const int motionIndex = static_cast<int>((*motion)["motionIndex"].value_or(int64_t{-1}));
            if (stateName.empty() || motionIndex < 0) continue;
            auto& positions = layout.blendTreeMotionPositions[stateName];
            if (positions.size() <= static_cast<size_t>(motionIndex))
                positions.resize(static_cast<size_t>(motionIndex) + 1);
            positions[static_cast<size_t>(motionIndex)] = {
                static_cast<float>((*motion)["x"].value_or(0.0)),
                static_cast<float>((*motion)["y"].value_or(0.0))
            };
        }
    }
    return layout;
}


// AnimationState 1 件を TOML へ書き出す。
// WHY: Base Layer (asset.states) と各 AnimationLayer::states の両方が同じ形式を使うため、
//      Save/Load へインライン展開せず 1 箇所に集約する。片方だけ直す事故を防ぐ。
toml::table WriteState(const scene::AnimationState& state)
{
    toml::table stateTable;
    stateTable.insert("name", state.name);
    stateTable.insert("mode", static_cast<int64_t>(state.mode));
    stateTable.insert("sourcePath", state.sourcePath);
    stateTable.insert("clipName", state.clipName);
    stateTable.insert("clipIndex", static_cast<int64_t>(state.clipIndex));
    stateTable.insert("speed", static_cast<double>(state.speed));
    stateTable.insert("loop", state.loop);
    stateTable.insert("ikWeight", static_cast<double>(state.ikWeight));

    toml::array transitions;
    for (const auto& transition : state.transitions)
        transitions.push_back(WriteTransition(transition));
    stateTable.insert("transitions", std::move(transitions));

    toml::table blendTree1D;
    blendTree1D.insert("paramName", state.blendTree1D.paramName);
    blendTree1D.insert("dampTime", static_cast<double>(state.blendTree1D.dampTime));
    blendTree1D.insert("syncNormalizedTime", state.blendTree1D.syncNormalizedTime);
    toml::array motions1D;
    for (const auto& motion : state.blendTree1D.motions)
        motions1D.push_back(WriteMotion(motion));
    blendTree1D.insert("motions", std::move(motions1D));
    stateTable.insert("blendTree1D", std::move(blendTree1D));

    toml::table blendTree2D;
    blendTree2D.insert("paramX", state.blendTree2D.paramX);
    blendTree2D.insert("paramY", state.blendTree2D.paramY);
    blendTree2D.insert("type", static_cast<int64_t>(state.blendTree2D.type));
    blendTree2D.insert("dampTime", static_cast<double>(state.blendTree2D.dampTime));
    blendTree2D.insert("syncNormalizedTime", state.blendTree2D.syncNormalizedTime);
    toml::array motions2D;
    for (const auto& motion : state.blendTree2D.motions)
        motions2D.push_back(WriteMotion(motion));
    blendTree2D.insert("motions", std::move(motions2D));
    stateTable.insert("blendTree2D", std::move(blendTree2D));
    return stateTable;
}

// TOML から AnimationState 1 件を読む。
scene::AnimationState ReadState(const toml::table& stateTable)
{
    scene::AnimationState state;
    state.name = stateTable["name"].value_or(std::string{});
    state.mode = static_cast<scene::AnimationStateMode>(
        stateTable["mode"].value_or(int64_t{0}));
    state.sourcePath = stateTable["sourcePath"].value_or(std::string{});
    state.clipName = stateTable["clipName"].value_or(std::string{});
    state.clipIndex = static_cast<int>(stateTable["clipIndex"].value_or(int64_t{-1}));
    state.speed = static_cast<float>(stateTable["speed"].value_or(1.0));
    state.loop = stateTable["loop"].value_or(true);
    state.ikWeight = static_cast<float>(stateTable["ikWeight"].value_or(1.0));

    if (const auto* transitions = stateTable["transitions"].as_array())
        for (const auto& transitionElement : *transitions)
            if (const auto* transitionTable = transitionElement.as_table())
                state.transitions.push_back(ReadTransition(*transitionTable));

    if (const auto* blend1D = stateTable["blendTree1D"].as_table()) {
        state.blendTree1D.paramName = (*blend1D)["paramName"].value_or(std::string{});
        state.blendTree1D.dampTime =
            static_cast<float>((*blend1D)["dampTime"].value_or(0.0));
        state.blendTree1D.syncNormalizedTime =
            (*blend1D)["syncNormalizedTime"].value_or(false);
        if (const auto* motions = (*blend1D)["motions"].as_array())
            for (const auto& motionElement : *motions)
                if (const auto* motionTable = motionElement.as_table())
                    state.blendTree1D.motions.push_back(ReadMotion(*motionTable));
    }
    if (const auto* blend2D = stateTable["blendTree2D"].as_table()) {
        state.blendTree2D.paramX = (*blend2D)["paramX"].value_or(std::string{});
        state.blendTree2D.paramY = (*blend2D)["paramY"].value_or(std::string{});
        state.blendTree2D.type = static_cast<scene::BlendTree2DType>(
            (*blend2D)["type"].value_or(int64_t{0}));
        state.blendTree2D.dampTime =
            static_cast<float>((*blend2D)["dampTime"].value_or(0.0));
        state.blendTree2D.syncNormalizedTime =
            (*blend2D)["syncNormalizedTime"].value_or(false);
        if (const auto* motions = (*blend2D)["motions"].as_array())
            for (const auto& motionElement : *motions)
                if (const auto* motionTable = motionElement.as_table())
                    state.blendTree2D.motions.push_back(ReadMotion(*motionTable));
    }
    return state;
}

} // namespace

bool SaveAnimatorControllerAsset(const std::string& path,
                                 const AnimatorControllerAsset& asset)
{
    toml::table root;
    root.insert("version", int64_t{6});
    root.insert("defaultStateName", asset.defaultStateName);

    toml::array states;
    for (const auto& state : asset.states)
        states.push_back(WriteState(state));
    root.insert("states", std::move(states));

    toml::array anyStateTransitions;
    for (const auto& transition : asset.anyStateTransitions)
        anyStateTransitions.push_back(WriteTransition(transition));
    root.insert("anyStateTransitions", std::move(anyStateTransitions));

    toml::array parameters;
    for (const auto& parameter : asset.parameters) {
        toml::table parameterTable;
        parameterTable.insert("name", parameter.name);
        parameterTable.insert("type", static_cast<int64_t>(parameter.type));
        parameterTable.insert("floatValue", static_cast<double>(parameter.floatValue));
        parameterTable.insert("intValue", static_cast<int64_t>(parameter.intValue));
        // Trigger は状態値ではなく一瞬の発火信号なので、Controller へ保存しない。
        // WHY: Editor の一時操作や古い .animcontroller の boolValue=true を復元すると、
        //      起動直後に Trigger 遷移が発火して意図しない State へ進んでしまう。
        parameterTable.insert(
            "boolValue",
            parameter.type == scene::ParamType::Trigger ? false : parameter.boolValue);
        parameters.push_back(std::move(parameterTable));
    }
    root.insert("parameters", std::move(parameters));

    toml::array layers;
    for (const auto& layer : asset.layers) {
        toml::table layerTable;
        layerTable.insert("name", layer.name);
        layerTable.insert("weight", static_cast<double>(layer.weight));
        layerTable.insert("mode", static_cast<int64_t>(layer.mode));
        layerTable.insert("enabled", layer.enabled);
        layerTable.insert("maskPath", layer.mask.path);

        // 加算レイヤーの基準ポーズ。
        toml::table additiveReference;
        additiveReference.insert("sourcePath", layer.additiveReference.sourcePath);
        additiveReference.insert("clipName", layer.additiveReference.clipName);
        additiveReference.insert("time", static_cast<double>(layer.additiveReference.time));
        layerTable.insert("additiveReference", std::move(additiveReference));

        // レイヤー独自ステートマシン。
        layerTable.insert("defaultStateName", layer.defaultStateName);
        toml::array layerStates;
        for (const auto& state : layer.states)
            layerStates.push_back(WriteState(state));
        layerTable.insert("states", std::move(layerStates));
        toml::array layerAnyState;
        for (const auto& transition : layer.anyStateTransitions)
            layerAnyState.push_back(WriteTransition(transition));
        layerTable.insert("anyStateTransitions", std::move(layerAnyState));

        // Slot は「今この瞬間割り込んでいるモーション」であってレイヤー定義ではないため、
        // ランタイム状態 (active / time / weight) は保存しない。既定のフェード時間だけ残す。
        toml::table slot;
        slot.insert("fadeInDuration", static_cast<double>(layer.slot.fadeInDuration));
        slot.insert("fadeOutDuration", static_cast<double>(layer.slot.fadeOutDuration));
        layerTable.insert("slot", std::move(slot));

        toml::array mappings;
        for (const auto& mapping : layer.retargetMappings) {
            toml::table mappingTable;
            mappingTable.insert("sourcePath", mapping.sourcePath);
            mappingTable.insert("targetPath", mapping.targetPath);
            mappingTable.insert("translationScale", static_cast<double>(mapping.translationScale));
            toml::array rotation;
            rotation.push_back(static_cast<double>(mapping.rotationOffset.x));
            rotation.push_back(static_cast<double>(mapping.rotationOffset.y));
            rotation.push_back(static_cast<double>(mapping.rotationOffset.z));
            rotation.push_back(static_cast<double>(mapping.rotationOffset.w));
            mappingTable.insert("rotationOffset", std::move(rotation));
            mappings.push_back(std::move(mappingTable));
        }
        layerTable.insert("retargetMappings", std::move(mappings));
        layers.push_back(std::move(layerTable));
    }
    root.insert("layers", std::move(layers));
    root.insert("baseLayerMaskPath", asset.baseLayerMaskPath);
    root.insert("editorLayout", WriteEditorLayout(asset.editorLayout));

    // .anim クリップ参照 (sourcePath) を guid: 形式で保存する (リネーム・移動耐性)。
    EncodeGuidRefs(root);

    std::ostringstream stream;
    stream << root;
    return util::FileSystem::WriteText(
        AssetManager::ResolveAssetPath(path), stream.str());
}

bool LoadAnimatorControllerAsset(const std::string& path,
                                 AnimatorControllerAsset& outAsset)
{
    std::string text;
    if (!util::FileSystem::ReadText(
            AssetManager::ResolveAssetPath(path), text)) return false;
    toml::parse_result result = toml::parse(text);
    if (!result) return false;

    // guid: 参照を "Assets/..." パスへ戻してから読む。
    DecodeGuidRefs(result.table());

    AnimatorControllerAsset loaded;
    loaded.defaultStateName = result["defaultStateName"].value_or(std::string{});
    if (const auto* states = result["states"].as_array()) {
        for (const auto& element : *states) {
            const auto* stateTable = element.as_table();
            if (!stateTable) continue;
            scene::AnimationState state = ReadState(*stateTable);

            loaded.states.push_back(std::move(state));
        }
    }
    if (const auto* transitions = result["anyStateTransitions"].as_array())
        for (const auto& element : *transitions)
            if (const auto* transitionTable = element.as_table())
                loaded.anyStateTransitions.push_back(ReadTransition(*transitionTable));

    if (const auto* parameters = result["parameters"].as_array()) {
        for (const auto& element : *parameters) {
            const auto* parameterTable = element.as_table();
            if (!parameterTable) continue;
            scene::AnimatorParameter parameter;
            parameter.name = (*parameterTable)["name"].value_or(std::string{});
            parameter.type = static_cast<scene::ParamType>(
                (*parameterTable)["type"].value_or(int64_t{0}));
            parameter.floatValue =
                static_cast<float>((*parameterTable)["floatValue"].value_or(0.0));
            parameter.intValue = static_cast<int>(
                (*parameterTable)["intValue"].value_or(int64_t{0}));
            parameter.boolValue = (*parameterTable)["boolValue"].value_or(false);
            loaded.parameters.push_back(std::move(parameter));
        }
    }
    if (const auto* layers = result["layers"].as_array()) {
        for (const auto& element : *layers) {
            const auto* layerTable = element.as_table();
            if (!layerTable) continue;
            scene::AnimationLayer layer;
            layer.name = (*layerTable)["name"].value_or(std::string{"Layer"});
            layer.weight = static_cast<float>((*layerTable)["weight"].value_or(1.0));
            layer.mode = static_cast<scene::AnimationLayerMode>(
                (*layerTable)["mode"].value_or(int64_t{0}));
            layer.enabled = (*layerTable)["enabled"].value_or(true);
            layer.mask.path = (*layerTable)["maskPath"].value_or(std::string{});
            if (const auto* additive = (*layerTable)["additiveReference"].as_table()) {
                layer.additiveReference.sourcePath =
                    (*additive)["sourcePath"].value_or(std::string{});
                layer.additiveReference.clipName =
                    (*additive)["clipName"].value_or(std::string{});
                layer.additiveReference.time =
                    static_cast<float>((*additive)["time"].value_or(0.0));
            }
            layer.defaultStateName = (*layerTable)["defaultStateName"].value_or(std::string{});
            if (const auto* layerStates = (*layerTable)["states"].as_array())
                for (const auto& stateElement : *layerStates)
                    if (const auto* stateTable = stateElement.as_table())
                        layer.states.push_back(ReadState(*stateTable));
            if (const auto* layerAnyState = (*layerTable)["anyStateTransitions"].as_array())
                for (const auto& transitionElement : *layerAnyState)
                    if (const auto* transitionTable = transitionElement.as_table())
                        layer.anyStateTransitions.push_back(ReadTransition(*transitionTable));
            if (const auto* slot = (*layerTable)["slot"].as_table()) {
                layer.slot.fadeInDuration =
                    static_cast<float>((*slot)["fadeInDuration"].value_or(0.15));
                layer.slot.fadeOutDuration =
                    static_cast<float>((*slot)["fadeOutDuration"].value_or(0.15));
            }

            if (const auto* mappings = (*layerTable)["retargetMappings"].as_array()) {
                for (const auto& mappingElement : *mappings) {
                    const auto* mappingTable = mappingElement.as_table();
                    if (!mappingTable) continue;
                    scene::RetargetBoneMapping mapping;
                    mapping.sourcePath = (*mappingTable)["sourcePath"].value_or(std::string{});
                    mapping.targetPath = (*mappingTable)["targetPath"].value_or(std::string{});
                    mapping.translationScale = static_cast<float>(
                        (*mappingTable)["translationScale"].value_or(1.0));
                    if (const auto* rotation = (*mappingTable)["rotationOffset"].as_array();
                        rotation && rotation->size() >= 4) {
                        mapping.rotationOffset = {
                            static_cast<float>((*rotation)[0].value_or(0.0)),
                            static_cast<float>((*rotation)[1].value_or(0.0)),
                            static_cast<float>((*rotation)[2].value_or(0.0)),
                            static_cast<float>((*rotation)[3].value_or(1.0))
                        };
                    }
                    layer.retargetMappings.push_back(std::move(mapping));
                }
            }
            loaded.layers.push_back(std::move(layer));
        }
    }
    loaded.baseLayerMaskPath = result["baseLayerMaskPath"].value_or(std::string{});
    if (const auto* editorLayout = result["editorLayout"].as_table())
        loaded.editorLayout = ReadEditorLayout(*editorLayout);
    outAsset = std::move(loaded);
    return true;
}

void ApplyAnimatorControllerAsset(const AnimatorControllerAsset& asset,
                                  scene::AnimatorComponent& animator)
{
    // Controller の初回ロードや差し替えでは再生状態を初期化するが、Graph の保存後に
    // 同じ Controller をライブ Animator へ反映する場合は、現在のモーションを止めない。
    // loadedControllerPath は AnimatorSystem が初回ロード完了後に設定するため、
    // 「初回ロード」と「編集反映」を安全に区別できる。
    const bool preservePlayback =
        !animator.loadedControllerPath.empty() &&
        animator.loadedControllerPath == animator.controllerPath;
    const std::string previousStateName = animator.currentStateName;
    const float previousStateTime = animator.stateTime;
    const std::string previousBlendToState = animator.blendToState;
    const float previousBlendToTime = animator.blendToTime;
    const float previousBlendWeight = animator.blendWeight;
    const float previousBlendDuration = animator.blendDuration;
    const auto previousLayers = animator.layers;

    animator.defaultStateName = asset.defaultStateName;
    animator.states = asset.states;
    animator.anyStateTransitions = asset.anyStateTransitions;
    animator.parameters = asset.parameters;
    // 旧形式・手編集された Controller に残る Trigger の true も実行開始前に捨てる。
    // Trigger は SetTrigger() でのみ発火し、アセットの初期値にはしない。
    for (auto& parameter : animator.parameters) {
        if (parameter.type == scene::ParamType::Trigger)
            parameter.boolValue = false;
    }
    animator.layers = asset.layers;
    animator.baseLayerMask.path = asset.baseLayerMaskPath;
    animator.baseLayerMask.Invalidate();
    animator.currentStateName.clear();
    animator.blendToState.clear();
    animator.stateTime = 0.0f;
    animator.blendToTime = 0.0f;
    animator.blendWeight = 0.0f;
    animator.blendDuration = 0.25f;
    animator.clips.clear();
    animator.clipSourcePaths.clear();
    animator.clipsLoaded = false;
    // clips を捨てるとルートモーションのサンプルキャッシュが持つ clip ポインタが無効になる。
    animator.rootMotionSamples.clear();

    const auto stateExists = [](const std::vector<scene::AnimationState>& states,
                                const std::string& name) {
        if (name.empty()) return false;
        for (const auto& state : states)
            if (state.name == name) return true;
        return false;
    };

    if (preservePlayback) {
        if (stateExists(animator.states, previousStateName)) {
            animator.currentStateName = previousStateName;
            animator.stateTime = previousStateTime;
            if (stateExists(animator.states, previousBlendToState)) {
                animator.blendToState = previousBlendToState;
                animator.blendToTime = previousBlendToTime;
                animator.blendWeight = previousBlendWeight;
                animator.blendDuration = previousBlendDuration;
            }
        }
    }

    // Layer の追加・名前変更だけで、既存 Layer のステート時間も巻き戻さない。
    // 初回 Controller 読込前に Script が PlayLayerState を呼んだ場合も、同名ステートが
    // Controller 側に存在するなら、その要求を復元して初回フレームから再生できる。
    for (auto& layer : animator.layers) {
        for (const auto& previousLayer : previousLayers) {
            if (previousLayer.name != layer.name) continue;
            if (stateExists(layer.states, previousLayer.runtime.currentStateName))
                layer.runtime = previousLayer.runtime;
            break;
        }
    }
}

AnimatorControllerAsset MakeAnimatorControllerAsset(
    const scene::AnimatorComponent& animator)
{
    AnimatorControllerAsset asset;
    asset.defaultStateName = animator.defaultStateName;
    asset.states = animator.states;
    asset.anyStateTransitions = animator.anyStateTransitions;
    asset.parameters = animator.parameters;
    asset.layers = animator.layers;
    asset.baseLayerMaskPath = animator.baseLayerMask.path;
    return asset;
}

} // namespace fbzz::asset
