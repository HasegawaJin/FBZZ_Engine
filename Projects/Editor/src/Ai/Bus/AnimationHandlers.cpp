/// @file    AnimationHandlers.cpp
/// @brief   Animator の照会・駆動と、ステートマシン / レイヤーの Undo 可能な編集 (animation.* / avatarMask.*)。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include "BusInternal.hpp"

#include <Editor/Util/UndoStack.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Engine/Asset/AvatarMaskAsset.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace fbzz::editor::ai::bus {

using scene::GameObject;
using scene::EntityID;

namespace {

scene::AnimatorComponent* FindAnimator(editor::EditorContext& ctx, const JsonValue& payload, Outcome& error)
{
    if (ctx.activeScene == nullptr) {
        error = Outcome::Err("NO_SCENE", "アクティブシーンがありません");
        return nullptr;
    }
    const std::string id = StringField(payload, "id");
    GameObject* go = ctx.activeScene->FindByGuid(id);
    if (go == nullptr) {
        error = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id);
        return nullptr;
    }
    scene::AnimatorComponent* animator = go->GetComponent<scene::AnimatorComponent>();
    if (animator == nullptr) error = Outcome::Err("NOT_PRESENT", "AnimatorComponent が装着されていません");
    return animator;
}

Outcome DoAnimationState(editor::EditorContext& ctx, const JsonValue& payload)
{
    Outcome error;
    scene::AnimatorComponent* animator = FindAnimator(ctx, payload, error);
    if (animator == nullptr) return error;
    JsonValue clips = JsonValue::MakeArray();
    for (size_t index = 0; index < animator->clips.size(); ++index) {
        const auto& clip = animator->clips[index];
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("index", JsonValue(static_cast<int>(index)));
        entry.Set("name", JsonValue(clip.name));
        entry.Set("duration", JsonValue(clip.GetDurationSeconds()));
        entry.Set("frameRate", JsonValue(clip.frameRate));
        clips.Push(std::move(entry));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("playing", JsonValue(animator->playing));
    result.Set("enabled", JsonValue(animator->enabled));
    result.Set("state", JsonValue(animator->currentStateName));
    result.Set("stateTime", JsonValue(animator->stateTime));
    result.Set("normalizedTime", JsonValue(animator->GetNormalizedTime()));
    result.Set("blendToState", JsonValue(animator->GetBlendToState()));
    result.Set("blendWeight", JsonValue(animator->blendWeight));
    result.Set("clips", std::move(clips));
    return Outcome::Ok(std::move(result));
}

/// @brief AnimatorSystem が直近フレームに評価したノード行列を、Skeleton の名前と親子情報付きで返す。
Outcome DoAnimationPose(editor::EditorContext& ctx, const JsonValue& payload)
{
    Outcome error;
    scene::AnimatorComponent* animator = FindAnimator(ctx, payload, error);
    if (animator == nullptr) return error;
    const std::string id = StringField(payload, "id");
    GameObject* go = ctx.activeScene->FindByGuid(id);
    scene::SkinnedMeshRenderer* renderer = go->GetComponent<scene::SkinnedMeshRenderer>();
    if (renderer == nullptr) {
        for (int index = 0; index < go->GetChildCount(); ++index) {
            GameObject* child = go->GetChild(index);
            if (child != nullptr && (renderer = child->GetComponent<scene::SkinnedMeshRenderer>()) != nullptr) break;
        }
    }
    if (renderer == nullptr || renderer->model == nullptr || renderer->model->skeleton == nullptr) {
        return Outcome::Err("SKELETON_NOT_READY", "SkinnedMeshRenderer の Skeleton がまだロードされていません");
    }
    const asset::Skeleton& skeleton = *renderer->model->skeleton;
    const JsonValue* limitValue = payload.Find("limit");
    const int limit = std::clamp(limitValue != nullptr && limitValue->IsNumber() ? limitValue->AsInt() : 128, 1, 512);
    const size_t available = std::min(skeleton.nodes.size(), animator->nodeGlobalTransforms.size());
    JsonValue nodes = JsonValue::MakeArray();
    for (size_t index = 0; index < available && static_cast<int>(index) < limit; ++index) {
        const asset::SkeletonNode& skeletonNode = skeleton.nodes[index];
        JsonValue node = JsonValue::MakeObject();
        node.Set("index", JsonValue(static_cast<int>(index)));
        node.Set("name", JsonValue(skeletonNode.name));
        node.Set("parentIndex", JsonValue(skeletonNode.parentIndex));
        node.Set("boneIndex", JsonValue(skeletonNode.boneIndex));
        node.Set("globalMatrix", MatrixToJson(animator->nodeGlobalTransforms[index]));
        nodes.Push(std::move(node));
    }
    JsonValue result = JsonValue::MakeObject();
    result.Set("id", JsonValue(id));
    result.Set("evaluated", JsonValue(!animator->nodeGlobalTransforms.empty()));
    result.Set("nodes", std::move(nodes));
    result.Set("count", JsonValue(static_cast<int>(std::min(available, static_cast<size_t>(limit)))));
    result.Set("total", JsonValue(static_cast<int>(available)));
    result.Set("truncated", JsonValue(available > static_cast<size_t>(limit)));
    return Outcome::Ok(std::move(result));
}

Outcome DoAnimationControl(editor::EditorContext& ctx, const JsonValue& payload, bool dryRun)
{
    Outcome error;
    scene::AnimatorComponent* animator = FindAnimator(ctx, payload, error);
    if (animator == nullptr) return error;
    const std::string action = StringField(payload, "action");
    const std::string stateName = StringField(payload, "state");
    const JsonValue* timeValue = payload.Find("time");
    const JsonValue* frameValue = payload.Find("frame");
    if (action != "play" && action != "pause" && action != "stop" && action != "seek") {
        return Outcome::Err("BAD_ARG", "未知の animation action: " + action);
    }
    if (action == "seek" && (timeValue == nullptr || !timeValue->IsNumber())
        && (frameValue == nullptr || !frameValue->IsNumber())) {
        return Outcome::Err("BAD_ARG", "seek には time または frame が必要です");
    }
    if (!stateName.empty()) {
        const auto iterator = std::find_if(animator->states.begin(), animator->states.end(), [&](const scene::AnimationState& state) {
            return state.name == stateName;
        });
        if (iterator == animator->states.end()) return Outcome::Err("STATE_NOT_FOUND", "Animator state が見つかりません: " + stateName);
    }
    if (dryRun) return DryRunPreview("animation.control:" + action);

    if (!stateName.empty()) {
        animator->currentStateName = stateName;
        animator->stateTime = 0.0f;
        animator->blendToState.clear();
        animator->blendToTime = 0.0f;
        animator->blendWeight = 0.0f;
    }

    if (action == "play") animator->playing = true;
    else if (action == "pause") animator->playing = false;
    else if (action == "stop") {
        animator->playing = false;
        animator->stateTime = 0.0f;
    } else if (action == "seek") {
        float seconds = timeValue != nullptr && timeValue->IsNumber() ? static_cast<float>(timeValue->AsNumber()) : -1.0f;
        if (frameValue != nullptr && frameValue->IsNumber()) {
            const asset::AnimationClip* clip = nullptr;
            const std::string activeStateName = stateName.empty() ? animator->currentStateName : stateName;
            const auto stateIterator = std::find_if(animator->states.begin(), animator->states.end(), [&](const scene::AnimationState& state) {
                return state.name == activeStateName;
            });
            if (stateIterator != animator->states.end()) {
                const scene::AnimationState& state = *stateIterator;
                if (!state.sourcePath.empty()) {
                    for (size_t index = 0; index < animator->clips.size(); ++index) {
                        if (index < animator->clipSourcePaths.size()
                            && animator->clipSourcePaths[index] == state.sourcePath
                            && (state.clipName.empty() || animator->clips[index].name == state.clipName)) {
                            clip = &animator->clips[index];
                            break;
                        }
                    }
                }
                if (clip == nullptr && !state.clipName.empty()) {
                    for (const auto& candidate : animator->clips) {
                        if (candidate.name == state.clipName) { clip = &candidate; break; }
                    }
                }
                if (clip == nullptr && state.clipIndex >= 0
                    && state.clipIndex < static_cast<int>(animator->clips.size())) {
                    clip = &animator->clips[static_cast<size_t>(state.clipIndex)];
                }
            }
            if (clip == nullptr || clip->frameRate <= 0.0f) return Outcome::Err("CLIP_NOT_READY", "frame seek にはロード済み clip が必要です");
            seconds = static_cast<float>(frameValue->AsNumber()) / clip->frameRate;
        }
        if (seconds < 0.0f) return Outcome::Err("BAD_ARG", "seek には time または frame が必要です");
        animator->stateTime = seconds;
    }
    return DoAnimationState(ctx, payload);
}

/// @name Animator ステートマシン照会・パラメーター駆動
/// @note animation.state は再生状態、animation.pose は骨行列だけで、どのパラメーターがどの遷移を発火させるかという構造は読めない。

const char* AnimatorParamTypeName(scene::ParamType type)
{
    switch (type) {
    case scene::ParamType::Float:   return "float";
    case scene::ParamType::Int:     return "int";
    case scene::ParamType::Bool:    return "bool";
    case scene::ParamType::Trigger: return "trigger";
    }
    return "unknown";
}

const char* AnimatorConditionOpName(scene::ConditionOp op)
{
    switch (op) {
    case scene::ConditionOp::Greater:  return "greater";
    case scene::ConditionOp::Less:     return "less";
    case scene::ConditionOp::Equal:    return "equal";
    case scene::ConditionOp::NotEqual: return "notEqual";
    case scene::ConditionOp::True:     return "true";
    case scene::ConditionOp::False:    return "false";
    }
    return "unknown";
}

/// @brief AnimatorConditionOpName の逆変換。未知文字列は false を返す。
bool ParseConditionOp(const std::string& name, scene::ConditionOp& out)
{
    if (name == "greater")  { out = scene::ConditionOp::Greater;  return true; }
    if (name == "less")     { out = scene::ConditionOp::Less;     return true; }
    if (name == "equal")    { out = scene::ConditionOp::Equal;    return true; }
    if (name == "notEqual") { out = scene::ConditionOp::NotEqual; return true; }
    if (name == "true")     { out = scene::ConditionOp::True;     return true; }
    if (name == "false")    { out = scene::ConditionOp::False;    return true; }
    return false;
}

/// @brief Bool/Trigger 用 (True/False) は閾値を使わない。それ以外 (Float/Int 比較) は threshold が有効。
bool ConditionOpUsesThreshold(scene::ConditionOp op)
{
    return op != scene::ConditionOp::True && op != scene::ConditionOp::False;
}

const char* AnimationStateModeName(scene::AnimationStateMode mode)
{
    switch (mode) {
    case scene::AnimationStateMode::Clip:        return "clip";
    case scene::AnimationStateMode::BlendTree1D: return "blendTree1D";
    case scene::AnimationStateMode::BlendTree2D: return "blendTree2D";
    }
    return "unknown";
}

JsonValue TransitionToJson(const scene::AnimationTransition& transition)
{
    JsonValue entry = JsonValue::MakeObject();
    entry.Set("toState", JsonValue(transition.toStateName));
    entry.Set("hasExitTime", JsonValue(transition.hasExitTime));
    entry.Set("exitTime", JsonValue(transition.exitTime));
    entry.Set("fixedDuration", JsonValue(transition.fixedDuration));
    entry.Set("transitionDuration", JsonValue(transition.transitionDuration));
    JsonValue conditions = JsonValue::MakeArray();
    for (const auto& condition : transition.conditions) {
        JsonValue conditionJson = JsonValue::MakeObject();
        conditionJson.Set("parameter", JsonValue(condition.paramName));
        conditionJson.Set("op", JsonValue(AnimatorConditionOpName(condition.op)));
        conditionJson.Set("threshold", JsonValue(condition.threshold));
        conditions.Push(std::move(conditionJson));
    }
    entry.Set("conditions", std::move(conditions));
    return entry;
}

/// @brief パラメーターの宣言型に応じた現在値を JSON へ書き込む (Trigger は bool として扱う)。
void SetAnimatorParamValueJson(JsonValue& target, const scene::AnimatorParameter& param)
{
    switch (param.type) {
    case scene::ParamType::Float:   target.Set("value", JsonValue(param.floatValue)); break;
    case scene::ParamType::Int:     target.Set("value", JsonValue(param.intValue)); break;
    case scene::ParamType::Bool:
    case scene::ParamType::Trigger: target.Set("value", JsonValue(param.boolValue)); break;
    }
}

/// @brief レイヤー 1 件を要約 JSON へ変換する (マスク・加算設定・ステート数・実行中ステートを含む)。
/// @note 書けるが読めない状態だと、作った構成を確認できず重複作成を繰り返すことになる。
JsonValue AnimationLayerToJson(const scene::AnimationLayer& layer)
{
    JsonValue entry = JsonValue::MakeObject();
    entry.Set("name", JsonValue(layer.name));
    entry.Set("enabled", JsonValue(layer.enabled));
    entry.Set("weight", JsonValue(layer.weight));
    entry.Set("mode", JsonValue(std::string(
        layer.mode == scene::AnimationLayerMode::Additive ? "additive" : "override")));
    entry.Set("maskPath", JsonValue(layer.mask.path));
    entry.Set("maskLoaded", JsonValue(layer.mask.loaded));

    /// @note 独自ステートマシンの有無で、どちらの再生方式かが決まる。
    entry.Set("hasOwnStateMachine", JsonValue(!layer.states.empty()));
    entry.Set("stateCount", JsonValue(static_cast<int>(layer.states.size())));
    entry.Set("defaultState", JsonValue(layer.defaultStateName));
    entry.Set("currentState", JsonValue(
        layer.states.empty() ? std::string{} : layer.runtime.currentStateName));
    entry.Set("blendToState", JsonValue(layer.runtime.blendToState));

    if (layer.mode == scene::AnimationLayerMode::Additive) {
        JsonValue additive = JsonValue::MakeObject();
        additive.Set("sourcePath", JsonValue(layer.additiveReference.sourcePath));
        additive.Set("clipName", JsonValue(layer.additiveReference.clipName));
        additive.Set("time", JsonValue(layer.additiveReference.time));
        entry.Set("additiveReference", std::move(additive));
    }

    JsonValue slot = JsonValue::MakeObject();
    slot.Set("active", JsonValue(layer.slot.active));
    slot.Set("stopping", JsonValue(layer.slot.stopping));
    slot.Set("weight", JsonValue(layer.slot.weight));
    slot.Set("clipName", JsonValue(layer.slot.clipName));
    slot.Set("sourcePath", JsonValue(layer.slot.sourcePath));
    entry.Set("slot", std::move(slot));
    return entry;
}

/// @name Avatar Mask (.mask)
/// @note レイヤーに maskPath を割り当てられても、マスク自体を作れなければ MCP から上半身/下半身の出し分けを完結できない。アセット単位で読み書きする。

/// @brief プロジェクト相対パスを解決し、.mask かどうかを検証する。
bool ResolveAvatarMaskPath(editor::EditorContext& ctx, const JsonValue& payload,
                           std::string& outAbsPath, Outcome& error)
{
    const std::string path = StringField(payload, "path");
    if (path.empty()) { error = Outcome::Err("BAD_ARG", "path が必要です"); return false; }
    const std::string lower = util::StringUtils::ToLower(util::FileSystem::GetExtension(path));
    if (lower != ".mask") {
        error = Outcome::Err("BAD_ARG", "path は .mask である必要があります: " + path);
        return false;
    }
    /// @note "Assets/..." 相対も絶対も受ける。projectRoot 配下へ閉じ込める。
    outAbsPath = util::FileSystem::IsChildPathText(path, ctx.projectRoot)
        ? path
        : util::FileSystem::PathToUtf8(
              util::FileSystem::PathFromUtf8(ctx.projectRoot) /
              util::FileSystem::PathFromUtf8(path));
    return true;
}

JsonValue AvatarMaskToJson(const asset::AvatarMaskAsset& mask, const std::string& path)
{
    JsonValue result = JsonValue::MakeObject();
    result.Set("path", JsonValue(path));
    result.Set("name", JsonValue(mask.name));
    result.Set("defaultInclude", JsonValue(mask.defaultInclude));
    result.Set("skeletonSourcePath", JsonValue(mask.skeletonSourcePath));
    JsonValue entries = JsonValue::MakeArray();
    for (const auto& e : mask.entries) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("bone", JsonValue(e.bonePath));
        entry.Set("weight", JsonValue(e.weight));
        entry.Set("includeChildren", JsonValue(e.includeChildren));
        entry.Set("blendDepth", JsonValue(e.blendDepth));
        entries.Push(std::move(entry));
    }
    result.Set("entries", std::move(entries));
    return result;
}

Outcome DoAvatarMaskGet(editor::EditorContext& ctx, const JsonValue& payload)
{
    std::string absPath;
    Outcome error;
    if (!ResolveAvatarMaskPath(ctx, payload, absPath, error)) return error;
    asset::AvatarMaskAsset mask;
    if (!asset::LoadAvatarMaskAsset(absPath, mask))
        return Outcome::Err("NOT_FOUND", "マスクを読み込めません: " + absPath);
    return Outcome::Ok(AvatarMaskToJson(mask, StringField(payload, "path")));
}

/// @brief マスクの作成と編集。Undo スタックには載せずファイルへ直接書く。
/// @note 他のアセット生成 (Controller / VFX) と同じ扱い。シーン状態ではないため UndoStack (シーン編集用) に混ぜると Undo の意味が食い違う。
Outcome DoAvatarMaskWrite(editor::EditorContext& ctx, const JsonValue& payload, bool dryRun)
{
    std::string absPath;
    Outcome error;
    if (!ResolveAvatarMaskPath(ctx, payload, absPath, error)) return error;

    const std::string action = StringField(payload, "action");
    static constexpr const char* kActions = "create/setBone/removeBone/clear/setDefaultInclude";
    if (action.empty())
        return Outcome::Err("BAD_ARG", std::string("action が必要です: ") + kActions);

    const bool exists = util::FileSystem::Exists(absPath);
    if (action == "create") {
        if (exists && !(payload.Find("overwrite") && payload.Find("overwrite")->IsBool()
                        && payload.Find("overwrite")->AsBool()))
            return Outcome::Err("ALREADY_EXISTS",
                                "既に存在します (overwrite=true で上書き): " + absPath);
    } else if (!exists) {
        return Outcome::Err("NOT_FOUND", "マスクが見つかりません: " + absPath);
    }

    asset::AvatarMaskAsset mask;
    if (exists && action != "create") {
        if (!asset::LoadAvatarMaskAsset(absPath, mask))
            return Outcome::Err("LOAD_FAILED", "マスクを読み込めません: " + absPath);
    }

    if (action == "create") {
        mask = asset::AvatarMaskAsset{};
        mask.name = util::FileSystem::GetFilename(absPath);
        if (const JsonValue* v = payload.Find("name"); v && v->IsString()) mask.name = v->AsString();
        if (const JsonValue* v = payload.Find("defaultInclude"); v && v->IsBool())
            mask.defaultInclude = v->AsBool();
        if (const JsonValue* v = payload.Find("skeletonSourcePath"); v && v->IsString())
            mask.skeletonSourcePath = v->AsString();
    } else if (action == "setDefaultInclude") {
        const JsonValue* v = payload.Find("defaultInclude");
        if (v == nullptr || !v->IsBool())
            return Outcome::Err("BAD_ARG", "defaultInclude (真偽) が必要です");
        mask.defaultInclude = v->AsBool();
    } else if (action == "clear") {
        mask.entries.clear();
    } else if (action == "setBone" || action == "removeBone") {
        const std::string bone = StringField(payload, "bone");
        if (bone.empty()) return Outcome::Err("BAD_ARG", "bone が必要です");

        auto it = std::find_if(mask.entries.begin(), mask.entries.end(),
            [&](const asset::AvatarMaskEntry& e) { return e.bonePath == bone; });

        if (action == "removeBone") {
            if (it == mask.entries.end())
                return Outcome::Err("BONE_NOT_FOUND", "エントリが見つかりません: " + bone);
            mask.entries.erase(it);
        } else {
            /// @note setBone は upsert。既存があれば指定キーだけ更新する。
            asset::AvatarMaskEntry prototype;
            if (it != mask.entries.end()) prototype = *it;
            prototype.bonePath = bone;
            if (const JsonValue* v = payload.Find("weight"); v && v->IsNumber())
                prototype.weight = std::clamp(static_cast<float>(v->AsNumber()), 0.0f, 1.0f);
            if (const JsonValue* v = payload.Find("includeChildren"); v && v->IsBool())
                prototype.includeChildren = v->AsBool();
            if (const JsonValue* v = payload.Find("blendDepth"); v && v->IsNumber())
                prototype.blendDepth = (std::max)(0, v->AsInt());
            if (it != mask.entries.end()) *it = prototype;
            else mask.entries.push_back(std::move(prototype));
        }
    } else {
        return Outcome::Err("BAD_ARG", std::string("未知の action: ") + action +
                                       " (" + kActions + ")");
    }

    if (dryRun) return DryRunPreview("avatarMask." + action);

    util::FileSystem::EnsureDirectory(util::FileSystem::GetDirectory(absPath));
    if (!asset::SaveAvatarMaskAsset(absPath, mask))
        return Outcome::Err("SAVE_FAILED", "マスクを保存できません: " + absPath);
    ctx.requestAssetBrowserRefresh = true;
    return Outcome::Ok(AvatarMaskToJson(mask, StringField(payload, "path")));
}

/// @brief AnimatorComponent のステートマシン全体 (states / transitions / parameters / anyState) を、現在の再生ステートやパラメーターのライブ値付きで返す。
Outcome DoAnimationGraph(editor::EditorContext& ctx, const JsonValue& payload)
{
    Outcome error;
    scene::AnimatorComponent* animator = FindAnimator(ctx, payload, error);
    if (animator == nullptr) return error;

    /// @note layer 指定があれば、そのレイヤーの独自ステートマシンを返す。省略で Base Layer。
    const std::string layerName = StringField(payload, "layer");
    const scene::AnimationLayer* targetLayer = nullptr;
    if (!layerName.empty()) {
        targetLayer = animator->FindLayer(layerName);
        if (targetLayer == nullptr)
            return Outcome::Err("LAYER_NOT_FOUND", "レイヤーが見つかりません: " + layerName);
    }

    const std::vector<scene::AnimationState>& targetStates =
        targetLayer ? targetLayer->states : animator->states;
    const std::vector<scene::AnimationTransition>& targetAnyState =
        targetLayer ? targetLayer->anyStateTransitions : animator->anyStateTransitions;
    const std::string& targetDefaultName =
        targetLayer ? targetLayer->defaultStateName : animator->defaultStateName;
    const std::string& targetCurrentName =
        targetLayer ? targetLayer->runtime.currentStateName : animator->currentStateName;

    JsonValue parameters = JsonValue::MakeArray();
    for (const auto& param : animator->parameters) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("name", JsonValue(param.name));
        entry.Set("type", JsonValue(AnimatorParamTypeName(param.type)));
        SetAnimatorParamValueJson(entry, param);
        parameters.Push(std::move(entry));
    }

    /// @note 実効デフォルトステート名 (未指定時は先頭)。Entry リンクの解決規則と一致させる。
    std::string defaultState = targetDefaultName;
    if (defaultState.empty() && !targetStates.empty())
        defaultState = targetStates.front().name;

    JsonValue states = JsonValue::MakeArray();
    for (const auto& state : targetStates) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("name", JsonValue(state.name));
        entry.Set("mode", JsonValue(AnimationStateModeName(state.mode)));
        entry.Set("clipName", JsonValue(state.clipName));
        entry.Set("sourcePath", JsonValue(state.sourcePath));
        entry.Set("speed", JsonValue(state.speed));
        entry.Set("loop", JsonValue(state.loop));
        entry.Set("ikWeight", JsonValue(state.ikWeight));
        entry.Set("isDefault", JsonValue(state.name == defaultState));
        entry.Set("isCurrent", JsonValue(state.name == targetCurrentName));
        /// @note BlendTree は駆動パラメーターとモーション数を要約表示する。
        if (state.mode == scene::AnimationStateMode::BlendTree1D) {
            entry.Set("blendParameter", JsonValue(state.blendTree1D.paramName));
            entry.Set("motionCount", JsonValue(static_cast<int>(state.blendTree1D.motions.size())));
        } else if (state.mode == scene::AnimationStateMode::BlendTree2D) {
            entry.Set("blendParameterX", JsonValue(state.blendTree2D.paramX));
            entry.Set("blendParameterY", JsonValue(state.blendTree2D.paramY));
            entry.Set("motionCount", JsonValue(static_cast<int>(state.blendTree2D.motions.size())));
        }
        JsonValue transitions = JsonValue::MakeArray();
        for (const auto& transition : state.transitions)
            transitions.Push(TransitionToJson(transition));
        entry.Set("transitions", std::move(transitions));
        states.Push(std::move(entry));
    }

    JsonValue anyStateTransitions = JsonValue::MakeArray();
    for (const auto& transition : targetAnyState)
        anyStateTransitions.Push(TransitionToJson(transition));

    /// @note layers はどのグラフを見ているかに関わらず常に返す。Base Layer を見に来て他レイヤーの存在を見落とすことを 1 回の問い合わせで防ぐ。
    JsonValue layers = JsonValue::MakeArray();
    for (const auto& layer : animator->layers)
        layers.Push(AnimationLayerToJson(layer));

    JsonValue result = JsonValue::MakeObject();
    result.Set("id", JsonValue(StringField(payload, "id")));
    result.Set("controllerPath", JsonValue(animator->controllerPath));
    /// @note どのグラフを返したか。空なら Base Layer。
    result.Set("layer", JsonValue(layerName));
    result.Set("defaultState", JsonValue(defaultState));
    result.Set("currentState", JsonValue(targetCurrentName));
    result.Set("blendToState", JsonValue(
        targetLayer ? targetLayer->runtime.blendToState : animator->blendToState));
    result.Set("blendWeight", JsonValue(
        targetLayer ? targetLayer->runtime.blendWeight : animator->blendWeight));
    result.Set("baseLayerMaskPath", JsonValue(animator->baseLayerMask.path));
    result.Set("parameters", std::move(parameters));
    result.Set("states", std::move(states));
    result.Set("anyStateTransitions", std::move(anyStateTransitions));
    result.Set("layers", std::move(layers));
    return Outcome::Ok(std::move(result));
}

/// @brief Animator パラメーターを名前で設定する。宣言型に合わせて value を解釈し、AnimatorSystem が次フレームに遷移や BlendTree へ反映する。
/// @note input.inject 経由の間接操作より直接的で、遷移条件や BlendTree の自律検証に使える。
Outcome DoAnimationSetParameter(editor::EditorContext& ctx, const JsonValue& payload, bool dryRun)
{
    Outcome error;
    scene::AnimatorComponent* animator = FindAnimator(ctx, payload, error);
    if (animator == nullptr) return error;

    const std::string name = StringField(payload, "name");
    if (name.empty()) return Outcome::Err("BAD_ARG", "パラメーター name が必要です");

    scene::AnimatorParameter* param = nullptr;
    for (auto& candidate : animator->parameters)
        if (candidate.name == name) { param = &candidate; break; }
    if (param == nullptr)
        return Outcome::Err("PARAM_NOT_FOUND", "Animator パラメーターが見つかりません: " + name);

    const JsonValue* valueField = payload.Find("value");

    /// @note 型ごとの入力検証。Trigger だけは value 省略で「発火」を許可する。
    switch (param->type) {
    case scene::ParamType::Float:
    case scene::ParamType::Int:
        if (valueField == nullptr || !valueField->IsNumber())
            return Outcome::Err("BAD_ARG", "数値 value が必要です (" + std::string(AnimatorParamTypeName(param->type)) + ")");
        break;
    case scene::ParamType::Bool:
        if (valueField == nullptr || !valueField->IsBool())
            return Outcome::Err("BAD_ARG", "真偽 value が必要です (bool)");
        break;
    case scene::ParamType::Trigger:
        /// @note 省略 = 発火 (true)。明示指定は bool のみ許可する (false でリセット可能)。
        if (valueField != nullptr && !valueField->IsBool())
            return Outcome::Err("BAD_ARG", "trigger の value は真偽のみ指定できます");
        break;
    }

    if (dryRun) return DryRunPreview("animation.setParameter:" + name);

    switch (param->type) {
    case scene::ParamType::Float:   param->floatValue = static_cast<float>(valueField->AsNumber()); break;
    case scene::ParamType::Int:     param->intValue = valueField->AsInt(); break;
    case scene::ParamType::Bool:    param->boolValue = valueField->AsBool(); break;
    case scene::ParamType::Trigger: param->boolValue = (valueField == nullptr) ? true : valueField->AsBool(); break;
    }

    JsonValue result = JsonValue::MakeObject();
    result.Set("id", JsonValue(StringField(payload, "id")));
    result.Set("name", JsonValue(param->name));
    result.Set("type", JsonValue(AnimatorParamTypeName(param->type)));
    SetAnimatorParamValueJson(result, *param);
    return Outcome::Ok(std::move(result));
}

/// @brief 指定ステートの BlendTree 構成を掘り下げて返す。
/// @note animation.graph はモーション数の要約に留めるが、こちらは各 Motion の source/clip/座標/speed/IK と直近フレームのランタイム Weight まで含める。
Outcome DoAnimationBlendTree(editor::EditorContext& ctx, const JsonValue& payload)
{
    Outcome error;
    scene::AnimatorComponent* animator = FindAnimator(ctx, payload, error);
    if (animator == nullptr) return error;

    const std::string stateName = StringField(payload, "state");
    if (stateName.empty()) return Outcome::Err("BAD_ARG", "state 名が必要です");

    const scene::AnimationState* state = nullptr;
    for (const auto& candidate : animator->states)
        if (candidate.name == stateName) { state = &candidate; break; }
    if (state == nullptr)
        return Outcome::Err("STATE_NOT_FOUND", "Animator state が見つかりません: " + stateName);
    if (state->mode == scene::AnimationStateMode::Clip)
        return Outcome::Err("NOT_BLEND_TREE", "このステートは BlendTree ではありません (mode=clip): " + stateName);

    /// @note 直近フレームの clipName→weight を引けるようにする (Motion への逆引き用)。
    auto runtimeWeight = [&](const std::string& clipName) -> const float* {
        for (const auto& entry : animator->currentBlendWeights)
            if (entry.first == clipName) return &entry.second;
        return nullptr;
    };

    auto motionToJson = [&](const scene::BlendTreeMotion& motion, bool is2D) {
        JsonValue entry = JsonValue::MakeObject();
        entry.Set("sourcePath", JsonValue(motion.sourcePath));
        entry.Set("clipName", JsonValue(motion.clipName));
        entry.Set("clipIndex", JsonValue(motion.clipIndex));
        if (is2D) {
            entry.Set("posX", JsonValue(motion.posX));
            entry.Set("posY", JsonValue(motion.posY));
        } else {
            entry.Set("threshold", JsonValue(motion.threshold));
        }
        entry.Set("speed", JsonValue(motion.speed));
        entry.Set("ikWeight", JsonValue(motion.ikWeight));
        if (const float* weight = runtimeWeight(motion.clipName))
            entry.Set("runtimeWeight", JsonValue(*weight));
        return entry;
    };

    JsonValue result = JsonValue::MakeObject();
    result.Set("id", JsonValue(StringField(payload, "id")));
    result.Set("state", JsonValue(stateName));
    result.Set("mode", JsonValue(AnimationStateModeName(state->mode)));
    result.Set("isCurrent", JsonValue(stateName == animator->currentStateName));

    JsonValue motions = JsonValue::MakeArray();
    if (state->mode == scene::AnimationStateMode::BlendTree1D) {
        const auto& tree = state->blendTree1D;
        result.Set("parameter", JsonValue(tree.paramName));
        result.Set("parameterValue", JsonValue(animator->GetFloat(tree.paramName)));
        result.Set("dampTime", JsonValue(tree.dampTime));
        result.Set("syncNormalizedTime", JsonValue(tree.syncNormalizedTime));
        for (const auto& motion : tree.motions) motions.Push(motionToJson(motion, false));
    } else {
        const auto& tree = state->blendTree2D;
        result.Set("parameterX", JsonValue(tree.paramX));
        result.Set("parameterY", JsonValue(tree.paramY));
        result.Set("parameterValueX", JsonValue(animator->GetFloat(tree.paramX)));
        result.Set("parameterValueY", JsonValue(animator->GetFloat(tree.paramY)));
        result.Set("blendType",
            JsonValue(tree.type == scene::BlendTree2DType::SimpleDirectional
                ? "simpleDirectional" : "freeformCartesian"));
        result.Set("dampTime", JsonValue(tree.dampTime));
        result.Set("syncNormalizedTime", JsonValue(tree.syncNormalizedTime));
        for (const auto& motion : tree.motions) motions.Push(motionToJson(motion, true));
    }
    result.Set("motions", std::move(motions));
    return Outcome::Ok(std::move(result));
}

/// @brief Animator ステートマシンの構造編集を Undo 可能な ICommand にまとめる共通ヘルパー。
/// @note どの編集も states/anyStateTransitions/parameters を丸ごとスナップショットして復元する (clip 実体を含まない軽量ベクトル)。
/// @note 復元後はランタイム遷移状態を消し、消えたステートを指したまま再生が続くのを防ぐ。
std::unique_ptr<ICommand> MakeAnimatorEditCommand(
    scene::Scene* scene, std::string id, const char* description,
    std::function<void()> markDirty,
    std::function<void(scene::AnimatorComponent&)> edit)
{
    auto beforeStates = std::make_shared<std::vector<scene::AnimationState>>();
    auto beforeAny = std::make_shared<std::vector<scene::AnimationTransition>>();
    auto beforeParams = std::make_shared<std::vector<scene::AnimatorParameter>>();
    /// @note layers もスナップショット対象に含める。レイヤーは独自のステートマシン・マスク参照・加算設定を持つ編集対象なので、含めないとレイヤー操作だけ Undo が効かなくなる。
    auto beforeLayers = std::make_shared<std::vector<scene::AnimationLayer>>();
    return std::make_unique<LambdaCommand>(description,
        [scene, id, edit, beforeStates, beforeAny, beforeParams, beforeLayers, markDirty]() {
            GameObject* g = scene->FindByGuid(id);
            if (!g) return;
            scene::AnimatorComponent* a = g->GetComponent<scene::AnimatorComponent>();
            if (!a) return;
            *beforeStates = a->states;
            *beforeAny = a->anyStateTransitions;
            *beforeParams = a->parameters;
            *beforeLayers = a->layers;
            edit(*a);
            markDirty();
        },
        [scene, id, beforeStates, beforeAny, beforeParams, beforeLayers, markDirty]() {
            GameObject* g = scene->FindByGuid(id);
            if (!g) return;
            scene::AnimatorComponent* a = g->GetComponent<scene::AnimatorComponent>();
            if (!a) return;
            a->states = *beforeStates;
            a->anyStateTransitions = *beforeAny;
            a->parameters = *beforeParams;
            a->layers = *beforeLayers;
            a->currentStateName.clear();
            a->blendToState.clear();
            a->stateTime = 0.0f;
            a->blendWeight = 0.0f;
            markDirty();
        });
}

/// @brief ステート編集の対象グラフ (Base Layer か、指定レイヤーの独自ステートマシンか) への参照。
/// @note animation.addState 等はどのグラフに対する操作かだけが違うため、レイヤー名の解決をここへ集約し各ハンドラは返ってきた配列を触るだけにする。
struct AnimatorGraphTarget {
    std::vector<scene::AnimationState>*      states       = nullptr;
    std::vector<scene::AnimationTransition>* anyState     = nullptr;
    std::string*                             defaultState = nullptr;
};

AnimatorGraphTarget ResolveGraphTarget(scene::AnimatorComponent& animator,
                                       const std::string& layerName)
{
    if (layerName.empty())
        return { &animator.states, &animator.anyStateTransitions, &animator.defaultStateName };
    scene::AnimationLayer* layer = animator.FindLayer(layerName);
    if (!layer) return {};
    return { &layer->states, &layer->anyStateTransitions, &layer->defaultStateName };
}

/// @brief Undo/Redo ラムダ内から使う、レイヤー解決つきのグラフ参照。
/// @note ラムダは実行時に GameObject を引き直すので対象レイヤーもその場で解決し、消えていたら Base Layer へ落とす (落ちるより Undo スタックを壊さない方が安全)。
struct AnimatorGraphRefs {
    scene::AnimatorComponent& animator;
    const std::string&        layerName;

    [[nodiscard]] std::vector<scene::AnimationState>& StatesRef() const
    {
        if (!layerName.empty())
            if (scene::AnimationLayer* l = animator.FindLayer(layerName)) return l->states;
        return animator.states;
    }
    [[nodiscard]] std::vector<scene::AnimationTransition>& AnyRef() const
    {
        if (!layerName.empty())
            if (scene::AnimationLayer* l = animator.FindLayer(layerName))
                return l->anyStateTransitions;
        return animator.anyStateTransitions;
    }
    [[nodiscard]] std::string& DefaultRef() const
    {
        if (!layerName.empty())
            if (scene::AnimationLayer* l = animator.FindLayer(layerName))
                return l->defaultStateName;
        return animator.defaultStateName;
    }
};

AnimatorGraphRefs GRAPH(scene::AnimatorComponent& animator, const std::string& layerName)
{
    return AnimatorGraphRefs{ animator, layerName };
}

/// @brief AnimationStateModeName の逆変換。未知文字列は false。
bool ParseAnimationStateMode(const std::string& name, scene::AnimationStateMode& out)
{
    if (name == "clip")        { out = scene::AnimationStateMode::Clip;        return true; }
    if (name == "blendTree1D") { out = scene::AnimationStateMode::BlendTree1D; return true; }
    if (name == "blendTree2D") { out = scene::AnimationStateMode::BlendTree2D; return true; }
    return false;
}

/// @brief BlendTree2D の座標解釈方式を文字列から解く。未知文字列は false。
bool ParseBlendTree2DType(const std::string& name, scene::BlendTree2DType& out)
{
    if (name == "simpleDirectional") { out = scene::BlendTree2DType::SimpleDirectional; return true; }
    if (name == "freeformCartesian") { out = scene::BlendTree2DType::FreeformCartesian; return true; }
    return false;
}

/// @brief 指定ステートを新名にリネームし、全遷移参照・defaultState・ランタイムステート名を追従させる。
void RenameAnimatorState(scene::AnimatorComponent& animator,
                         const std::string& oldName, const std::string& newName)
{
    for (auto& state : animator.states) {
        if (state.name == oldName) state.name = newName;
        for (auto& transition : state.transitions)
            if (transition.toStateName == oldName) transition.toStateName = newName;
    }
    for (auto& transition : animator.anyStateTransitions)
        if (transition.toStateName == oldName) transition.toStateName = newName;
    if (animator.defaultStateName == oldName) animator.defaultStateName = newName;
    if (animator.currentStateName == oldName) animator.currentStateName = newName;
    if (animator.blendToState == oldName) animator.blendToState = newName;
}

std::unique_ptr<ICommand> BuildAnimatorGraphCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    /// @name Animator ステートマシンの構造編集 (Undo 対応)
    /// @note すべて MakeAnimatorEditCommand 経由で states/anyState/parameters をスナップショット復元する。
    if (type == "animation.addTransition" || type == "animation.setCondition" ||
        type == "animation.removeTransition" ||
        type == "animation.addState" || type == "animation.setState" ||
        type == "animation.removeState" ||
        type == "animation.addMotion" || type == "animation.setMotion" ||
        type == "animation.removeMotion" ||
        type == "animation.addParameter" || type == "animation.removeParameter") {
        const std::string id = StringField(payload, "id");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        scene::AnimatorComponent* animator = go->GetComponent<scene::AnimatorComponent>();
        if (animator == nullptr) { err = Outcome::Err("NOT_PRESENT", "AnimatorComponent が装着されていません"); return nullptr; }

        /// @note レイヤー指定 (省略で Base Layer)。上半身レイヤー等に独自の遷移グラフを組めるよう、編集操作がどのグラフに対するものかを選べるようにする。
        const std::string layerName = StringField(payload, "layer");
        const AnimatorGraphTarget graphTarget = ResolveGraphTarget(*animator, layerName);
        if (graphTarget.states == nullptr) {
            err = Outcome::Err("LAYER_NOT_FOUND", "レイヤーが見つかりません: " + layerName);
            return nullptr;
        }

        auto findStateIndex = [&](const std::string& name) -> int {
            for (int i = 0; i < static_cast<int>((*graphTarget.states).size()); ++i)
                if ((*graphTarget.states)[static_cast<size_t>(i)].name == name) return i;
            return -1;
        };

        /// @name パラメーター CRUD
        if (type == "animation.addParameter") {
            const std::string name = StringField(payload, "name");
            if (name.empty()) { err = Outcome::Err("BAD_ARG", "パラメーター name が必要です"); return nullptr; }
            const bool duplicate = std::any_of(animator->parameters.begin(), animator->parameters.end(),
                [&](const scene::AnimatorParameter& p) { return p.name == name; });
            if (duplicate) { err = Outcome::Err("DUPLICATE_PARAM", "同名のパラメーターが既に存在します: " + name); return nullptr; }

            scene::AnimatorParameter prototype;
            prototype.name = name;
            /// @note type 省略時は float。value 省略時は型ごとの既定 (0 / false)。
            const std::string typeName = StringField(payload, "type");
            if (!typeName.empty()) {
                if (typeName == "float")        prototype.type = scene::ParamType::Float;
                else if (typeName == "int")     prototype.type = scene::ParamType::Int;
                else if (typeName == "bool")    prototype.type = scene::ParamType::Bool;
                else if (typeName == "trigger") prototype.type = scene::ParamType::Trigger;
                else { err = Outcome::Err("BAD_ARG", "type は float/int/bool/trigger のいずれかです"); return nullptr; }
            }
            if (const JsonValue* v = payload.Find("value"); v != nullptr) {
                switch (prototype.type) {
                case scene::ParamType::Float:
                    if (!v->IsNumber()) { err = Outcome::Err("BAD_ARG", "float の value は数値です"); return nullptr; }
                    prototype.floatValue = static_cast<float>(v->AsNumber());
                    break;
                case scene::ParamType::Int:
                    if (!v->IsNumber()) { err = Outcome::Err("BAD_ARG", "int の value は数値です"); return nullptr; }
                    prototype.intValue = v->AsInt();
                    break;
                case scene::ParamType::Bool:
                case scene::ParamType::Trigger:
                    if (!v->IsBool()) { err = Outcome::Err("BAD_ARG", "bool/trigger の value は真偽です"); return nullptr; }
                    prototype.boolValue = v->AsBool();
                    break;
                }
            }
            return MakeAnimatorEditCommand(scene, id, "AI: Add Animator Parameter", markDirty,
                [prototype](scene::AnimatorComponent& a) { a.parameters.push_back(prototype); });
        }

        if (type == "animation.removeParameter") {
            const std::string name = StringField(payload, "name");
            if (name.empty()) { err = Outcome::Err("BAD_ARG", "パラメーター name が必要です"); return nullptr; }
            const bool exists = std::any_of(animator->parameters.begin(), animator->parameters.end(),
                [&](const scene::AnimatorParameter& p) { return p.name == name; });
            if (!exists) { err = Outcome::Err("PARAM_NOT_FOUND", "パラメーターが見つかりません: " + name); return nullptr; }
            /// @note このパラメーターを参照する遷移条件は残す (Unity/本エディター同様)。無効参照は発火しないだけで壊れず、Undo で丸ごと戻せるため cascade 削除しない。
            return MakeAnimatorEditCommand(scene, id, "AI: Remove Animator Parameter", markDirty,
                [name](scene::AnimatorComponent& a) {
                    a.parameters.erase(
                        std::remove_if(a.parameters.begin(), a.parameters.end(),
                            [&](const scene::AnimatorParameter& p) { return p.name == name; }),
                        a.parameters.end());
                });
        }

        /// @name ステート削除 (参照の遷移も掃除)
        if (type == "animation.removeState") {
            const std::string stateName = StringField(payload, "state");
            if (stateName.empty()) { err = Outcome::Err("BAD_ARG", "対象 state 名が必要です"); return nullptr; }
            if (findStateIndex(stateName) < 0) { err = Outcome::Err("STATE_NOT_FOUND", "ステートが見つかりません: " + stateName); return nullptr; }
            return MakeAnimatorEditCommand(scene, id, "AI: Remove Animator State", markDirty,
                [stateName, layerName](scene::AnimatorComponent& a) {
                    /// @note 対象ステートを消し、他ステート/AnyState からの遷移参照も除去する (パネルの DeleteState 相当)。
                    GRAPH(a, layerName).StatesRef().erase(
                        std::remove_if(GRAPH(a, layerName).StatesRef().begin(), GRAPH(a, layerName).StatesRef().end(),
                            [&](const scene::AnimationState& s) { return s.name == stateName; }),
                        GRAPH(a, layerName).StatesRef().end());
                    for (auto& s : GRAPH(a, layerName).StatesRef()) {
                        s.transitions.erase(
                            std::remove_if(s.transitions.begin(), s.transitions.end(),
                                [&](const scene::AnimationTransition& t) { return t.toStateName == stateName; }),
                            s.transitions.end());
                    }
                    GRAPH(a, layerName).AnyRef().erase(
                        std::remove_if(GRAPH(a, layerName).AnyRef().begin(), GRAPH(a, layerName).AnyRef().end(),
                            [&](const scene::AnimationTransition& t) { return t.toStateName == stateName; }),
                        GRAPH(a, layerName).AnyRef().end());
                    if (GRAPH(a, layerName).DefaultRef() == stateName)
                        GRAPH(a, layerName).DefaultRef() = GRAPH(a, layerName).StatesRef().empty() ? std::string{} : GRAPH(a, layerName).StatesRef().front().name;
                });
        }

        /// @name ステート追加
        if (type == "animation.addState") {
            const std::string name = StringField(payload, "name");
            if (name.empty()) { err = Outcome::Err("BAD_ARG", "ステート name が必要です"); return nullptr; }
            if (findStateIndex(name) >= 0) { err = Outcome::Err("DUPLICATE_STATE", "同名のステートが既に存在します: " + name); return nullptr; }

            scene::AnimationState prototype;
            prototype.name = name;
            if (const JsonValue* v = payload.Find("mode"); v != nullptr && v->IsString()) {
                if (!ParseAnimationStateMode(v->AsString(), prototype.mode)) {
                    err = Outcome::Err("BAD_ARG", "mode は clip/blendTree1D/blendTree2D のいずれかです"); return nullptr;
                }
            }
            if (const JsonValue* v = payload.Find("sourcePath"); v != nullptr && v->IsString()) prototype.sourcePath = v->AsString();
            if (const JsonValue* v = payload.Find("clipName"); v != nullptr && v->IsString()) prototype.clipName = v->AsString();
            if (const JsonValue* v = payload.Find("clipIndex"); v != nullptr && v->IsNumber()) prototype.clipIndex = v->AsInt();
            if (const JsonValue* v = payload.Find("speed"); v != nullptr && v->IsNumber()) prototype.speed = static_cast<float>(v->AsNumber());
            if (const JsonValue* v = payload.Find("loop"); v != nullptr && v->IsBool()) prototype.loop = v->AsBool();
            if (const JsonValue* v = payload.Find("ikWeight"); v != nullptr && v->IsNumber()) prototype.ikWeight = static_cast<float>(v->AsNumber());
            if (const JsonValue* v = payload.Find("blendParameter"); v != nullptr && v->IsString()) prototype.blendTree1D.paramName = v->AsString();
            if (const JsonValue* v = payload.Find("blendParameterX"); v != nullptr && v->IsString()) prototype.blendTree2D.paramX = v->AsString();
            if (const JsonValue* v = payload.Find("blendParameterY"); v != nullptr && v->IsString()) prototype.blendTree2D.paramY = v->AsString();
            bool setAsDefault = false;
            if (const JsonValue* v = payload.Find("setAsDefault"); v != nullptr && v->IsBool()) setAsDefault = v->AsBool();

            return MakeAnimatorEditCommand(scene, id, "AI: Add Animator State", markDirty,
                [prototype, setAsDefault, layerName](scene::AnimatorComponent& a) {
                    GRAPH(a, layerName).StatesRef().push_back(prototype);
                    /// @note Unity 同様、最初のステートや明示指定時はデフォルトにする。
                    if (setAsDefault || GRAPH(a, layerName).DefaultRef().empty())
                        GRAPH(a, layerName).DefaultRef() = prototype.name;
                });
        }

        /// @name ステート更新 (rename は参照追従)
        if (type == "animation.setState") {
            const std::string stateName = StringField(payload, "state");
            if (stateName.empty()) { err = Outcome::Err("BAD_ARG", "対象 state 名が必要です"); return nullptr; }
            const int stateIndex = findStateIndex(stateName);
            if (stateIndex < 0) { err = Outcome::Err("STATE_NOT_FOUND", "ステートが見つかりません: " + stateName); return nullptr; }

            /// @note rename 検証: 新名は空不可、他ステートと衝突不可。
            std::string newName;
            if (const JsonValue* v = payload.Find("name"); v != nullptr && v->IsString()) {
                newName = v->AsString();
                if (newName.empty()) { err = Outcome::Err("BAD_ARG", "name は空にできません"); return nullptr; }
                const int existing = findStateIndex(newName);
                if (existing >= 0 && existing != stateIndex) { err = Outcome::Err("DUPLICATE_STATE", "同名のステートが既に存在します: " + newName); return nullptr; }
            }
            /// @note mode / 2Dtype は build 時にパース検証する。
            bool hasMode = false;
            scene::AnimationStateMode mode = scene::AnimationStateMode::Clip;
            if (const JsonValue* v = payload.Find("mode"); v != nullptr && v->IsString()) {
                if (!ParseAnimationStateMode(v->AsString(), mode)) { err = Outcome::Err("BAD_ARG", "mode は clip/blendTree1D/blendTree2D のいずれかです"); return nullptr; }
                hasMode = true;
            }
            bool hasBlend2DType = false;
            scene::BlendTree2DType blend2DType = scene::BlendTree2DType::SimpleDirectional;
            if (const JsonValue* v = payload.Find("blend2DType"); v != nullptr && v->IsString()) {
                if (!ParseBlendTree2DType(v->AsString(), blend2DType)) { err = Outcome::Err("BAD_ARG", "blend2DType は simpleDirectional/freeformCartesian のいずれかです"); return nullptr; }
                hasBlend2DType = true;
            }
            /// @note 変更内容を JSON からそのまま持ち回すために payload を複製して束縛する。
            auto captured = std::make_shared<JsonValue>(payload);
            const bool setAsDefault = [&]() {
                const JsonValue* v = payload.Find("setAsDefault");
                return v != nullptr && v->IsBool() && v->AsBool();
            }();

            return MakeAnimatorEditCommand(scene, id, "AI: Set Animator State", markDirty,
                [stateName, newName, hasMode, mode, hasBlend2DType, blend2DType, setAsDefault, captured, layerName]
                (scene::AnimatorComponent& a) {
                    int index = -1;
                    for (int i = 0; i < static_cast<int>(GRAPH(a, layerName).StatesRef().size()); ++i)
                        if (GRAPH(a, layerName).StatesRef()[static_cast<size_t>(i)].name == stateName) { index = i; break; }
                    if (index < 0) return;
                    scene::AnimationState& s = GRAPH(a, layerName).StatesRef()[static_cast<size_t>(index)];
                    const JsonValue& p = *captured;
                    if (hasMode) s.mode = mode;
                    if (const JsonValue* v = p.Find("sourcePath"); v != nullptr && v->IsString()) s.sourcePath = v->AsString();
                    if (const JsonValue* v = p.Find("clipName"); v != nullptr && v->IsString()) s.clipName = v->AsString();
                    if (const JsonValue* v = p.Find("clipIndex"); v != nullptr && v->IsNumber()) s.clipIndex = v->AsInt();
                    if (const JsonValue* v = p.Find("speed"); v != nullptr && v->IsNumber()) s.speed = static_cast<float>(v->AsNumber());
                    if (const JsonValue* v = p.Find("loop"); v != nullptr && v->IsBool()) s.loop = v->AsBool();
                    if (const JsonValue* v = p.Find("ikWeight"); v != nullptr && v->IsNumber()) s.ikWeight = static_cast<float>(v->AsNumber());
                    if (const JsonValue* v = p.Find("blendParameter"); v != nullptr && v->IsString()) s.blendTree1D.paramName = v->AsString();
                    if (const JsonValue* v = p.Find("blendParameterX"); v != nullptr && v->IsString()) s.blendTree2D.paramX = v->AsString();
                    if (const JsonValue* v = p.Find("blendParameterY"); v != nullptr && v->IsString()) s.blendTree2D.paramY = v->AsString();
                    if (hasBlend2DType) s.blendTree2D.type = blend2DType;
                    if (setAsDefault) GRAPH(a, layerName).DefaultRef() = s.name;
                    /// @note rename は参照追従のため最後に行う (s は無効になり得るので name 取得後)。
                    if (!newName.empty() && newName != stateName)
                        RenameAnimatorState(a, stateName, newName);
                });
        }

        /// @name BlendTree Motion 追加・更新・削除
        if (type == "animation.addMotion" || type == "animation.setMotion" ||
            type == "animation.removeMotion") {
            const std::string stateName = StringField(payload, "state");
            if (stateName.empty()) { err = Outcome::Err("BAD_ARG", "対象 state 名が必要です"); return nullptr; }
            const int stateIndex = findStateIndex(stateName);
            if (stateIndex < 0) { err = Outcome::Err("STATE_NOT_FOUND", "ステートが見つかりません: " + stateName); return nullptr; }
            const scene::AnimationState& state = (*graphTarget.states)[static_cast<size_t>(stateIndex)];
            const bool is2D = state.mode == scene::AnimationStateMode::BlendTree2D;
            if (state.mode != scene::AnimationStateMode::BlendTree1D && !is2D) {
                err = Outcome::Err("NOT_BLEND_TREE", "このステートは BlendTree ではありません: " + stateName); return nullptr;
            }
            const std::vector<scene::BlendTreeMotion>& motions =
                is2D ? state.blendTree2D.motions : state.blendTree1D.motions;

            const bool isAdd = (type == "animation.addMotion");
            const bool isRemove = (type == "animation.removeMotion");
            int motionIndex = -1;
            if (!isAdd) {
                const JsonValue* motionIndexField = payload.Find("motionIndex");
                if (motionIndexField == nullptr || !motionIndexField->IsNumber()) { err = Outcome::Err("BAD_ARG", "motionIndex が必要です"); return nullptr; }
                motionIndex = motionIndexField->AsInt();
                if (motionIndex < 0 || motionIndex >= static_cast<int>(motions.size())) { err = Outcome::Err("MOTION_NOT_FOUND", "motionIndex が範囲外です"); return nullptr; }
            }
            auto captured = std::make_shared<JsonValue>(payload);

            const char* description = isAdd ? "AI: Add BlendTree Motion"
                : isRemove ? "AI: Remove BlendTree Motion" : "AI: Set BlendTree Motion";
            return MakeAnimatorEditCommand(scene, id, description, markDirty,
                [stateName, is2D, isAdd, isRemove, motionIndex, captured, layerName](scene::AnimatorComponent& a) {
                    int index = -1;
                    for (int i = 0; i < static_cast<int>(GRAPH(a, layerName).StatesRef().size()); ++i)
                        if (GRAPH(a, layerName).StatesRef()[static_cast<size_t>(i)].name == stateName) { index = i; break; }
                    if (index < 0) return;
                    scene::AnimationState& s = GRAPH(a, layerName).StatesRef()[static_cast<size_t>(index)];
                    std::vector<scene::BlendTreeMotion>& motionList =
                        is2D ? s.blendTree2D.motions : s.blendTree1D.motions;

                    if (isRemove) {
                        if (motionIndex >= 0 && motionIndex < static_cast<int>(motionList.size()))
                            motionList.erase(motionList.begin() + motionIndex);
                        return;
                    }

                    scene::BlendTreeMotion* motion = nullptr;
                    scene::BlendTreeMotion added;
                    if (isAdd) {
                        motion = &added;
                    } else {
                        if (motionIndex < 0 || motionIndex >= static_cast<int>(motionList.size())) return;
                        motion = &motionList[static_cast<size_t>(motionIndex)];
                    }
                    const JsonValue& p = *captured;
                    if (const JsonValue* v = p.Find("sourcePath"); v != nullptr && v->IsString()) motion->sourcePath = v->AsString();
                    if (const JsonValue* v = p.Find("clipName"); v != nullptr && v->IsString()) motion->clipName = v->AsString();
                    if (const JsonValue* v = p.Find("clipIndex"); v != nullptr && v->IsNumber()) motion->clipIndex = v->AsInt();
                    if (const JsonValue* v = p.Find("threshold"); v != nullptr && v->IsNumber()) motion->threshold = static_cast<float>(v->AsNumber());
                    if (const JsonValue* v = p.Find("posX"); v != nullptr && v->IsNumber()) motion->posX = static_cast<float>(v->AsNumber());
                    if (const JsonValue* v = p.Find("posY"); v != nullptr && v->IsNumber()) motion->posY = static_cast<float>(v->AsNumber());
                    if (const JsonValue* v = p.Find("speed"); v != nullptr && v->IsNumber()) motion->speed = static_cast<float>(v->AsNumber());
                    if (const JsonValue* v = p.Find("ikWeight"); v != nullptr && v->IsNumber()) motion->ikWeight = static_cast<float>(v->AsNumber());
                    if (isAdd) motionList.push_back(added);
                });
        }

        /// @note from 省略 = Any State 遷移。指定時はそのステートの transitions を対象にする。
        const JsonValue* fromField = payload.Find("from");
        const bool isAnyState = (fromField == nullptr) || !fromField->IsString()
            || fromField->AsString().empty() || fromField->AsString() == "AnyState";
        const std::string fromName = isAnyState ? std::string{} : fromField->AsString();
        int fromIndex = -1;
        if (!isAnyState) {
            fromIndex = findStateIndex(fromName);
            if (fromIndex < 0) { err = Outcome::Err("STATE_NOT_FOUND", "from ステートが見つかりません: " + fromName); return nullptr; }
        }

        /// @note 対象の transitions ベクトルを返す (build 時の検証用。実行時は再解決する)。
        auto sourceTransitions = [&]() -> std::vector<scene::AnimationTransition>& {
            return isAnyState ? (*graphTarget.anyState)
                              : (*graphTarget.states)[static_cast<size_t>(fromIndex)].transitions;
        };

        /// @name 遷移削除
        if (type == "animation.removeTransition") {
            const JsonValue* transitionIndexField = payload.Find("transitionIndex");
            if (transitionIndexField == nullptr || !transitionIndexField->IsNumber()) { err = Outcome::Err("BAD_ARG", "transitionIndex が必要です"); return nullptr; }
            const int transitionIndex = transitionIndexField->AsInt();
            if (transitionIndex < 0 || transitionIndex >= static_cast<int>(sourceTransitions().size())) { err = Outcome::Err("TRANSITION_NOT_FOUND", "transitionIndex が範囲外です"); return nullptr; }
            return MakeAnimatorEditCommand(scene, id, "AI: Remove Animator Transition", markDirty,
                [isAnyState, fromName, transitionIndex, layerName](scene::AnimatorComponent& a) {
                    std::vector<scene::AnimationTransition>* transitions = nullptr;
                    if (isAnyState) transitions = &GRAPH(a, layerName).AnyRef();
                    else for (auto& s : GRAPH(a, layerName).StatesRef()) if (s.name == fromName) { transitions = &s.transitions; break; }
                    if (transitions == nullptr || transitionIndex >= static_cast<int>(transitions->size())) return;
                    transitions->erase(transitions->begin() + transitionIndex);
                });
        }

        if (type == "animation.addTransition") {
            const std::string toName = StringField(payload, "to");
            if (toName.empty()) { err = Outcome::Err("BAD_ARG", "to ステート名が必要です"); return nullptr; }
            if (findStateIndex(toName) < 0) { err = Outcome::Err("STATE_NOT_FOUND", "to ステートが見つかりません: " + toName); return nullptr; }
            for (const auto& existing : sourceTransitions())
                if (existing.toStateName == toName) { err = Outcome::Err("DUPLICATE_TRANSITION", "同じ遷移先が既に存在します: " + toName); return nullptr; }

            /// @note 任意の初期設定を build 時に検証しておく。
            scene::AnimationTransition prototype;
            prototype.toStateName = toName;
            if (const JsonValue* v = payload.Find("hasExitTime"); v != nullptr && v->IsBool()) prototype.hasExitTime = v->AsBool();
            if (const JsonValue* v = payload.Find("exitTime"); v != nullptr && v->IsNumber()) prototype.exitTime = static_cast<float>(v->AsNumber());
            if (const JsonValue* v = payload.Find("fixedDuration"); v != nullptr && v->IsBool()) prototype.fixedDuration = v->AsBool();
            if (const JsonValue* v = payload.Find("transitionDuration"); v != nullptr && v->IsNumber()) prototype.transitionDuration = static_cast<float>(v->AsNumber());

            return MakeAnimatorEditCommand(scene, id, "AI: Add Animator Transition", markDirty,
                [isAnyState, fromName, prototype, layerName](scene::AnimatorComponent& a) {
                    if (isAnyState) { GRAPH(a, layerName).AnyRef().push_back(prototype); }
                    else {
                        for (auto& s : GRAPH(a, layerName).StatesRef())
                            if (s.name == fromName) { s.transitions.push_back(prototype); break; }
                    }
                });
        }

        /// @note animation.setCondition: 指定 transition の conditions を add/update/remove/clear する。
        const JsonValue* transitionIndexField = payload.Find("transitionIndex");
        if (transitionIndexField == nullptr || !transitionIndexField->IsNumber()) {
            err = Outcome::Err("BAD_ARG", "transitionIndex が必要です"); return nullptr;
        }
        const int transitionIndex = transitionIndexField->AsInt();
        if (transitionIndex < 0 || transitionIndex >= static_cast<int>(sourceTransitions().size())) {
            err = Outcome::Err("TRANSITION_NOT_FOUND", "transitionIndex が範囲外です"); return nullptr;
        }
        const std::string action = StringField(payload, "action");
        if (action != "add" && action != "update" && action != "remove" && action != "clear") {
            err = Outcome::Err("BAD_ARG", "action は add/update/remove/clear のいずれかです"); return nullptr;
        }

        scene::AnimationTransition& targetTransition = sourceTransitions()[static_cast<size_t>(transitionIndex)];

        /// @note add / update は条件の中身を build 時に検証する。
        scene::AnimatorCondition prototype;
        int conditionIndex = -1;
        if (action == "add" || action == "update") {
            const std::string paramName = StringField(payload, "parameter");
            if (paramName.empty()) { err = Outcome::Err("BAD_ARG", "parameter 名が必要です"); return nullptr; }
            const bool paramExists = std::any_of(animator->parameters.begin(), animator->parameters.end(),
                [&](const scene::AnimatorParameter& p) { return p.name == paramName; });
            if (!paramExists) { err = Outcome::Err("PARAM_NOT_FOUND", "パラメーターが見つかりません: " + paramName); return nullptr; }
            scene::ConditionOp op = scene::ConditionOp::Greater;
            if (!ParseConditionOp(StringField(payload, "op"), op)) {
                err = Outcome::Err("BAD_ARG", "op は greater/less/equal/notEqual/true/false のいずれかです"); return nullptr;
            }
            prototype.paramName = paramName;
            prototype.op = op;
            if (ConditionOpUsesThreshold(op)) {
                const JsonValue* thresholdField = payload.Find("threshold");
                if (thresholdField != nullptr && thresholdField->IsNumber())
                    prototype.threshold = static_cast<float>(thresholdField->AsNumber());
            }
        }
        if (action == "update" || action == "remove") {
            const JsonValue* conditionIndexField = payload.Find("conditionIndex");
            if (conditionIndexField == nullptr || !conditionIndexField->IsNumber()) {
                err = Outcome::Err("BAD_ARG", "conditionIndex が必要です"); return nullptr;
            }
            conditionIndex = conditionIndexField->AsInt();
            if (conditionIndex < 0 || conditionIndex >= static_cast<int>(targetTransition.conditions.size())) {
                err = Outcome::Err("CONDITION_NOT_FOUND", "conditionIndex が範囲外です"); return nullptr;
            }
        }

        return MakeAnimatorEditCommand(scene, id, "AI: Edit Animator Condition", markDirty,
            [isAnyState, fromName, transitionIndex, action, prototype, conditionIndex, layerName]
            (scene::AnimatorComponent& a) {
                std::vector<scene::AnimationTransition>* transitions = nullptr;
                if (isAnyState) transitions = &GRAPH(a, layerName).AnyRef();
                else for (auto& s : GRAPH(a, layerName).StatesRef()) if (s.name == fromName) { transitions = &s.transitions; break; }
                if (transitions == nullptr || transitionIndex >= static_cast<int>(transitions->size())) return;
                auto& conditions = (*transitions)[static_cast<size_t>(transitionIndex)].conditions;
                if (action == "add") conditions.push_back(prototype);
                else if (action == "clear") conditions.clear();
                else if (action == "remove") {
                    if (conditionIndex < static_cast<int>(conditions.size()))
                        conditions.erase(conditions.begin() + conditionIndex);
                } else if (action == "update") {
                    if (conditionIndex < static_cast<int>(conditions.size()))
                        conditions[static_cast<size_t>(conditionIndex)] = prototype;
                }
            });
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}

std::unique_ptr<ICommand> BuildAnimatorLayerCommand([[maybe_unused]] editor::EditorContext& ctx, [[maybe_unused]] const std::string& type,
    [[maybe_unused]] const JsonValue& payload, [[maybe_unused]] Outcome& err,
    [[maybe_unused]] std::shared_ptr<std::string> createdSink, [[maybe_unused]] JsonValue* detailSink)
{
    scene::Scene* scene = ctx.activeScene;
    if (scene == nullptr) { err = Outcome::Err("NO_SCENE", "アクティブシーンがありません"); return nullptr; }
    [[maybe_unused]] const auto markDirty = [&ctx]() { if (ctx.markSceneDirty) ctx.markSceneDirty(); };

    /// @name Animator レイヤーと Slot (上半身 / 下半身の出し分け)
    /// @note レイヤーを作れないと animation.addState 等の layer 指定が使えない。レイヤー CRUD と Slot 再生をここに揃え、MCP から一連の操作を完結させる。
    if (type == "animation.addLayer" || type == "animation.setLayer" ||
        type == "animation.removeLayer" ||
        type == "animation.playSlot" || type == "animation.stopSlot") {
        const std::string id = StringField(payload, "id");
        GameObject* go = scene->FindByGuid(id);
        if (go == nullptr) { err = Outcome::Err("NODE_NOT_FOUND", "NodeId が見つかりません: " + id); return nullptr; }
        scene::AnimatorComponent* animator = go->GetComponent<scene::AnimatorComponent>();
        if (animator == nullptr) { err = Outcome::Err("NOT_PRESENT", "AnimatorComponent が装着されていません"); return nullptr; }

        const auto parseLayerMode = [&](const std::string& name, scene::AnimationLayerMode& out) {
            if (name == "override") { out = scene::AnimationLayerMode::Override; return true; }
            if (name == "additive") { out = scene::AnimationLayerMode::Additive; return true; }
            return false;
        };

        if (type == "animation.addLayer") {
            const std::string name = StringField(payload, "name");
            if (name.empty()) { err = Outcome::Err("BAD_ARG", "レイヤー name が必要です"); return nullptr; }
            if (animator->FindLayer(name) != nullptr) {
                err = Outcome::Err("DUPLICATE_LAYER", "同名のレイヤーが既に存在します: " + name);
                return nullptr;
            }
            scene::AnimationLayer prototype;
            prototype.name = name;
            if (const JsonValue* v = payload.Find("weight"); v && v->IsNumber())
                prototype.weight = std::clamp(static_cast<float>(v->AsNumber()), 0.0f, 1.0f);
            if (const JsonValue* v = payload.Find("mode"); v && v->IsString()) {
                if (!parseLayerMode(v->AsString(), prototype.mode)) {
                    err = Outcome::Err("BAD_ARG", "mode は override/additive のいずれかです");
                    return nullptr;
                }
            }
            if (const JsonValue* v = payload.Find("enabled"); v && v->IsBool()) prototype.enabled = v->AsBool();
            if (const JsonValue* v = payload.Find("maskPath"); v && v->IsString()) prototype.mask.path = v->AsString();
            if (const JsonValue* v = payload.Find("additiveSourcePath"); v && v->IsString())
                prototype.additiveReference.sourcePath = v->AsString();
            if (const JsonValue* v = payload.Find("additiveClipName"); v && v->IsString())
                prototype.additiveReference.clipName = v->AsString();
            if (const JsonValue* v = payload.Find("additiveTime"); v && v->IsNumber())
                prototype.additiveReference.time = static_cast<float>(v->AsNumber());

            return MakeAnimatorEditCommand(scene, id, "AI: Add Animator Layer", markDirty,
                [prototype](scene::AnimatorComponent& a) { a.layers.push_back(prototype); });
        }

        /// @note 以降は既存レイヤーを対象にする。
        const std::string layerName = StringField(payload, "layer");
        if (layerName.empty()) { err = Outcome::Err("BAD_ARG", "対象 layer 名が必要です"); return nullptr; }
        if (animator->FindLayer(layerName) == nullptr) {
            err = Outcome::Err("LAYER_NOT_FOUND", "レイヤーが見つかりません: " + layerName);
            return nullptr;
        }

        if (type == "animation.removeLayer") {
            return MakeAnimatorEditCommand(scene, id, "AI: Remove Animator Layer", markDirty,
                [layerName](scene::AnimatorComponent& a) {
                    a.layers.erase(
                        std::remove_if(a.layers.begin(), a.layers.end(),
                            [&](const scene::AnimationLayer& l) { return l.name == layerName; }),
                        a.layers.end());
                });
        }

        if (type == "animation.setLayer") {
            /// @note rename 検証: 空不可・他レイヤーと衝突不可。
            std::string newName;
            if (const JsonValue* v = payload.Find("name"); v && v->IsString()) {
                newName = v->AsString();
                if (newName.empty()) { err = Outcome::Err("BAD_ARG", "name は空にできません"); return nullptr; }
                if (newName != layerName && animator->FindLayer(newName) != nullptr) {
                    err = Outcome::Err("DUPLICATE_LAYER", "同名のレイヤーが既に存在します: " + newName);
                    return nullptr;
                }
            }
            scene::AnimationLayerMode mode{};
            bool hasMode = false;
            if (const JsonValue* v = payload.Find("mode"); v && v->IsString()) {
                if (!parseLayerMode(v->AsString(), mode)) {
                    err = Outcome::Err("BAD_ARG", "mode は override/additive のいずれかです");
                    return nullptr;
                }
                hasMode = true;
            }
            /// @note 部分更新: 指定されたキーだけを書き換える。
            const JsonValue* weightValue   = payload.Find("weight");
            const JsonValue* enabledValue  = payload.Find("enabled");
            const JsonValue* maskValue     = payload.Find("maskPath");
            const JsonValue* defaultValue  = payload.Find("defaultStateName");
            const JsonValue* addSrcValue   = payload.Find("additiveSourcePath");
            const JsonValue* addClipValue  = payload.Find("additiveClipName");
            const JsonValue* addTimeValue  = payload.Find("additiveTime");

            const float weight = (weightValue && weightValue->IsNumber())
                ? std::clamp(static_cast<float>(weightValue->AsNumber()), 0.0f, 1.0f) : 0.0f;
            const bool  enabled = (enabledValue && enabledValue->IsBool()) && enabledValue->AsBool();
            const std::string maskPath = (maskValue && maskValue->IsString()) ? maskValue->AsString() : std::string{};
            const std::string defaultStateName = (defaultValue && defaultValue->IsString()) ? defaultValue->AsString() : std::string{};
            const std::string addSrc = (addSrcValue && addSrcValue->IsString()) ? addSrcValue->AsString() : std::string{};
            const std::string addClip = (addClipValue && addClipValue->IsString()) ? addClipValue->AsString() : std::string{};
            const float addTime = (addTimeValue && addTimeValue->IsNumber()) ? static_cast<float>(addTimeValue->AsNumber()) : 0.0f;

            const bool hasWeight  = weightValue && weightValue->IsNumber();
            const bool hasEnabled = enabledValue && enabledValue->IsBool();
            const bool hasMask    = maskValue && maskValue->IsString();
            const bool hasDefault = defaultValue && defaultValue->IsString();
            const bool hasAddSrc  = addSrcValue && addSrcValue->IsString();
            const bool hasAddClip = addClipValue && addClipValue->IsString();
            const bool hasAddTime = addTimeValue && addTimeValue->IsNumber();

            return MakeAnimatorEditCommand(scene, id, "AI: Set Animator Layer", markDirty,
                [=](scene::AnimatorComponent& a) {
                    scene::AnimationLayer* l = a.FindLayer(layerName);
                    if (l == nullptr) return;
                    if (hasWeight)  l->weight = weight;
                    if (hasEnabled) l->enabled = enabled;
                    if (hasMode)    l->mode = mode;
                    if (hasMask) {
                        l->mask.path = maskPath;
                        /// @note 次フレームの AnimatorSystem に読み直させる。
                        l->mask.Invalidate();
                    }
                    if (hasDefault) l->defaultStateName = defaultStateName;
                    if (hasAddSrc)  l->additiveReference.sourcePath = addSrc;
                    if (hasAddClip) l->additiveReference.clipName = addClip;
                    if (hasAddTime) l->additiveReference.time = addTime;
                    if (!newName.empty()) l->name = newName;
                });
        }

        /// @name Slot
        /// @note Slot はランタイム状態だが、他の編集と同じ Command 経路に載せる。dryRun 判定と Undo への積み込みを呼び出し側へ任せられる。
        if (type == "animation.playSlot") {
            const std::string sourcePath = StringField(payload, "sourcePath");
            const std::string clipName   = StringField(payload, "clipName");
            if (sourcePath.empty() && clipName.empty()) {
                err = Outcome::Err("BAD_ARG", "sourcePath または clipName が必要です");
                return nullptr;
            }
            float fadeIn = 0.15f, fadeOut = 0.15f, slotSpeed = 1.0f;
            bool  slotLoop = false;
            if (const JsonValue* v = payload.Find("fadeIn"); v && v->IsNumber())  fadeIn = static_cast<float>(v->AsNumber());
            if (const JsonValue* v = payload.Find("fadeOut"); v && v->IsNumber()) fadeOut = static_cast<float>(v->AsNumber());
            if (const JsonValue* v = payload.Find("speed"); v && v->IsNumber())   slotSpeed = static_cast<float>(v->AsNumber());
            if (const JsonValue* v = payload.Find("loop"); v && v->IsBool())      slotLoop = v->AsBool();
            return MakeAnimatorEditCommand(scene, id, "AI: Play Animator Slot", markDirty,
                [=](scene::AnimatorComponent& a) {
                    a.PlaySlot(layerName, sourcePath, clipName, fadeIn, fadeOut, slotSpeed, slotLoop);
                });
        }

        if (type == "animation.stopSlot") {
            float fadeOut = -1.0f;
            if (const JsonValue* v = payload.Find("fadeOut"); v && v->IsNumber())
                fadeOut = static_cast<float>(v->AsNumber());
            return MakeAnimatorEditCommand(scene, id, "AI: Stop Animator Slot", markDirty,
                [layerName, fadeOut](scene::AnimatorComponent& a) {
                    a.StopSlot(layerName, fadeOut);
                });
        }
    }

    err = Outcome::Err("UNSUPPORTED", "この Command は transaction 内で使用できません: " + type);
    return nullptr;
}
} // namespace

void RegisterAnimationHandlers(BusHandlerTable& table)
{
    table.AddQuery("animation.state", [](BusCall& call) { return DoAnimationState(call.ctx, call.payload); });
    table.AddQuery("animation.graph", [](BusCall& call) { return DoAnimationGraph(call.ctx, call.payload); });
    table.AddQuery("animation.blendTree", [](BusCall& call) { return DoAnimationBlendTree(call.ctx, call.payload); });
    table.AddQuery("animation.pose", [](BusCall& call) { return DoAnimationPose(call.ctx, call.payload); });
    table.AddQuery("avatarMask.get", [](BusCall& call) { return DoAvatarMaskGet(call.ctx, call.payload); });

    table.AddCommand("animation.control", [](BusCall& call) { return DoAnimationControl(call.ctx, call.payload, call.dryRun); });
    table.AddCommand("animation.setParameter", [](BusCall& call) { return DoAnimationSetParameter(call.ctx, call.payload, call.dryRun); });
    /// @note Avatar Mask はアセットファイル操作。シーンの UndoStack には載せない。
    table.AddCommand("avatarMask.write", [](BusCall& call) { return DoAvatarMaskWrite(call.ctx, call.payload, call.dryRun); });

    for (const char* type : { "animation.addTransition", "animation.setCondition", "animation.removeTransition",
                               "animation.addState", "animation.setState", "animation.removeState",
                               "animation.addMotion", "animation.setMotion", "animation.removeMotion",
                               "animation.addParameter", "animation.removeParameter" }) {
        table.AddBuilder(type, BuildAnimatorGraphCommand);
    }
    for (const char* type : { "animation.addLayer", "animation.setLayer", "animation.removeLayer",
                               "animation.playSlot", "animation.stopSlot" }) {
        table.AddBuilder(type, BuildAnimatorLayerCommand);
    }
}

} // namespace fbzz::editor::ai::bus
