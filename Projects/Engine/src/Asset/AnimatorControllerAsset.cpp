// FBZZ Engine
// AnimatorControllerAsset.cpp | fbzz::asset
// .animcontroller の TOML 入出力と AnimatorComponent への適用
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/AssetManager.hpp>
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
    // 旧 Controller の transitionDuration は秒指定なので true を既定値にする。
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

} // namespace

bool SaveAnimatorControllerAsset(const std::string& path,
                                 const AnimatorControllerAsset& asset)
{
    toml::table root;
    root.insert("version", int64_t{4});
    root.insert("defaultStateName", asset.defaultStateName);

    toml::array clipSources;
    for (const auto& source : asset.clipSources) clipSources.push_back(source);
    root.insert("clipSources", std::move(clipSources));

    toml::array states;
    for (const auto& state : asset.states) {
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
        toml::array motions1D;
        for (const auto& motion : state.blendTree1D.motions)
            motions1D.push_back(WriteMotion(motion));
        blendTree1D.insert("motions", std::move(motions1D));
        stateTable.insert("blendTree1D", std::move(blendTree1D));

        toml::table blendTree2D;
        blendTree2D.insert("paramX", state.blendTree2D.paramX);
        blendTree2D.insert("paramY", state.blendTree2D.paramY);
        blendTree2D.insert("type", static_cast<int64_t>(state.blendTree2D.type));
        toml::array motions2D;
        for (const auto& motion : state.blendTree2D.motions)
            motions2D.push_back(WriteMotion(motion));
        blendTree2D.insert("motions", std::move(motions2D));
        stateTable.insert("blendTree2D", std::move(blendTree2D));
        states.push_back(std::move(stateTable));
    }
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
        parameterTable.insert("boolValue", parameter.boolValue);
        parameters.push_back(std::move(parameterTable));
    }
    root.insert("parameters", std::move(parameters));

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
    const toml::parse_result result = toml::parse(text);
    if (!result) return false;

    AnimatorControllerAsset loaded;
    loaded.defaultStateName = result["defaultStateName"].value_or(std::string{});
    if (const auto* sources = result["clipSources"].as_array()) {
        for (const auto& element : *sources)
            if (const auto value = element.value<std::string>())
                loaded.clipSources.push_back(*value);
    }
    if (const auto* states = result["states"].as_array()) {
        for (const auto& element : *states) {
            const auto* stateTable = element.as_table();
            if (!stateTable) continue;
            scene::AnimationState state;
            state.name = (*stateTable)["name"].value_or(std::string{});
            state.mode = static_cast<scene::AnimationStateMode>(
                (*stateTable)["mode"].value_or(int64_t{0}));
            state.sourcePath =
                (*stateTable)["sourcePath"].value_or(std::string{});
            state.clipName = (*stateTable)["clipName"].value_or(std::string{});
            state.clipIndex = static_cast<int>(
                (*stateTable)["clipIndex"].value_or(int64_t{-1}));
            state.speed = static_cast<float>((*stateTable)["speed"].value_or(1.0));
            state.loop = (*stateTable)["loop"].value_or(true);
            state.ikWeight = static_cast<float>((*stateTable)["ikWeight"].value_or(1.0));

            if (const auto* transitions = (*stateTable)["transitions"].as_array())
                for (const auto& transitionElement : *transitions)
                    if (const auto* transitionTable = transitionElement.as_table())
                        state.transitions.push_back(ReadTransition(*transitionTable));

            if (const auto* blend1D = (*stateTable)["blendTree1D"].as_table()) {
                state.blendTree1D.paramName =
                    (*blend1D)["paramName"].value_or(std::string{});
                state.blendTree1D.dampTime =
                    static_cast<float>((*blend1D)["dampTime"].value_or(0.0));
                if (const auto* motions = (*blend1D)["motions"].as_array())
                    for (const auto& motionElement : *motions)
                        if (const auto* motionTable = motionElement.as_table())
                            state.blendTree1D.motions.push_back(ReadMotion(*motionTable));
            }
            if (const auto* blend2D = (*stateTable)["blendTree2D"].as_table()) {
                state.blendTree2D.paramX = (*blend2D)["paramX"].value_or(std::string{});
                state.blendTree2D.paramY = (*blend2D)["paramY"].value_or(std::string{});
                state.blendTree2D.type = static_cast<scene::BlendTree2DType>(
                    (*blend2D)["type"].value_or(int64_t{0}));
                if (const auto* motions = (*blend2D)["motions"].as_array())
                    for (const auto& motionElement : *motions)
                        if (const auto* motionTable = motionElement.as_table())
                            state.blendTree2D.motions.push_back(ReadMotion(*motionTable));
            }

            // version 1 Controller は Source を全体配列と clipIndex で保持していた。
            // Node 側 Source が空なら旧 index を Source 配列へ対応付けて自動移行する。
            auto migrateSource = [&loaded](std::string& sourcePath, int clipIndex) {
                if (!sourcePath.empty() || loaded.clipSources.empty()) return;
                int remainingIndex = clipIndex;
                for (const auto& legacySource : loaded.clipSources) {
                    const auto model = AssetManager::Load<Model>(legacySource);
                    if (!model) continue;
                    const int clipCount = static_cast<int>(model->clips.size());
                    if (remainingIndex >= 0 && remainingIndex < clipCount) {
                        sourcePath = legacySource;
                        return;
                    }
                    remainingIndex -= clipCount;
                }
            };
            migrateSource(state.sourcePath, state.clipIndex);
            for (auto& motion : state.blendTree1D.motions)
                migrateSource(motion.sourcePath, motion.clipIndex);
            for (auto& motion : state.blendTree2D.motions)
                migrateSource(motion.sourcePath, motion.clipIndex);
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
    outAsset = std::move(loaded);
    return true;
}

void ApplyAnimatorControllerAsset(const AnimatorControllerAsset& asset,
                                  scene::AnimatorComponent& animator)
{
    animator.clipSources = asset.clipSources;
    animator.defaultStateName = asset.defaultStateName;
    animator.states = asset.states;
    animator.anyStateTransitions = asset.anyStateTransitions;
    animator.parameters = asset.parameters;
    animator.currentStateName.clear();
    animator.blendToState.clear();
    animator.stateTime = 0.0f;
    animator.clips.clear();
    animator.clipSourcePaths.clear();
    animator.clipsLoaded = false;
}

AnimatorControllerAsset MakeAnimatorControllerAsset(
    const scene::AnimatorComponent& animator)
{
    AnimatorControllerAsset asset;
    asset.defaultStateName = animator.defaultStateName;
    asset.states = animator.states;
    // 旧 Embedded Animator からControllerを作る場合だけ index をNode Sourceへ移行する。
    auto migrateSource = [&animator](std::string& sourcePath, int clipIndex) {
        if (!sourcePath.empty()) return;
        int remainingIndex = clipIndex;
        for (const auto& legacySource : animator.clipSources) {
            const auto model = AssetManager::Load<Model>(legacySource);
            if (!model) continue;
            const int clipCount = static_cast<int>(model->clips.size());
            if (remainingIndex >= 0 && remainingIndex < clipCount) {
                sourcePath = legacySource;
                return;
            }
            remainingIndex -= clipCount;
        }
    };
    for (auto& state : asset.states) {
        migrateSource(state.sourcePath, state.clipIndex);
        for (auto& motion : state.blendTree1D.motions)
            migrateSource(motion.sourcePath, motion.clipIndex);
        for (auto& motion : state.blendTree2D.motions)
            migrateSource(motion.sourcePath, motion.clipIndex);
    }
    asset.anyStateTransitions = animator.anyStateTransitions;
    asset.parameters = animator.parameters;
    return asset;
}

} // namespace fbzz::asset
