// FBZZ Engine
// AnimationGraphPanel.cpp | fbzz::editor
// AnimatorComponent のステートマシンをノードグラフとして編集するパネル
// WHAT: imnodes で State ノードと Transition リンクを描画し、AnimatorComponent を直接更新する。
#include <Editor/Panels/AnimationGraphPanel.hpp>
#include <Editor/Panels/AnimationGraphInspector.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AnimatorControllerAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/GraphLayout.hpp>
#include <Editor/GraphLayoutSerializer.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imnodes.h>
#include <imgui_internal.h>
// WHY: fbzz_editor は STATIC lib で、Sandbox / GameHub の最終リンク設定に
//      imnodes.lib が伝播しない古い VS プロジェクトでも LNK2019 を出さないため、
//      AnimationGraphPanel.obj に imnodes の実装を同梱する。
#include <imnodes.cpp>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::editor {

namespace {

std::uint64_t& AnimationGraphEditGeneration()
{
    static std::uint64_t generation = 0;
    return generation;
}

// Play Mode 中はグラフをランタイム監視専用にし、Scene/Undo データを書き換えない。
bool CanEditAnimationGraph(const EditorContext& ctx)
{
    return ctx.playMode == nullptr || ctx.playMode->IsInEditor();
}

constexpr float SIDEBAR_WIDTH = 260.0f;
constexpr float MIN_CANVAS_ZOOM = 0.50f;
constexpr float MAX_CANVAS_ZOOM = 1.80f;
constexpr float ZOOM_STEP = 0.10f;
constexpr float WHEEL_PAN_STEP = 56.0f;
constexpr float BASE_NODE_CARD_WIDTH = 188.0f;
constexpr ImGuiHoveredFlags CANVAS_HOVER_FLAGS =
    ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem;

const char* ParamTypeName(scene::ParamType type)
{
    static constexpr const char* NAMES[] = { "Float", "Int", "Bool", "Trigger" };
    return NAMES[std::clamp(static_cast<int>(type), 0, 3)];
}

const char* ConditionOpName(scene::ConditionOp op)
{
    static constexpr const char* NAMES[] = { "Greater", "Less", "Equal", "NotEqual", "True", "False" };
    return NAMES[std::clamp(static_cast<int>(op), 0, 5)];
}

void MarkDirty(EditorContext& ctx)
{
    if (!CanEditAnimationGraph(ctx))
        return;

    ++AnimationGraphEditGeneration();
    if (util::StringUtils::EndsWith(
            ctx.selectedAssetPath, ".fbzzanimcontroller")) {
        ctx.animationControllerDirty = true;
        // Registry に登録し Save All / 終了時確認で一括保存できるようにする
        const std::string capturedPath = ctx.selectedAssetPath;
        std::weak_ptr<scene::AnimatorComponent> weakAnimator = ctx.animationControllerEditor;
        AssetDirtyRegistry::Register(
            capturedPath, NormalizeAssetPath(capturedPath), "CTRL",
            [capturedPath, weakAnimator]() {
                auto animator = weakAnimator.lock();
                if (!animator) return false;
                const auto controller = asset::MakeAnimatorControllerAsset(*animator);
                return asset::SaveAnimatorControllerAsset(capturedPath, controller);
            });
        return;
    }
    if (ctx.markSceneDirty) ctx.markSceneDirty();
}

struct AnimationGraphUndoTracker {
    std::string owner;
    ImGuiID activeId = 0;
    scene::EntityID entityId;
    std::shared_ptr<scene::AnimatorComponent> assetAnimator;
    scene::AnimatorComponent beforeAnimator;
    GraphLayout beforeLayout;
    bool active = false;
};

// Animation Graph が編集する定義だけを複製し、巨大なClipトラックや骨行列をUndoへ含めない。
// WHY: AnimatorComponent全体の毎フレーム深いコピーは、Clip読込後にEditor描画を大幅に重くする。
scene::AnimatorComponent MakeAnimationGraphSnapshot(
    const scene::AnimatorComponent& source)
{
    scene::AnimatorComponent snapshot;
    snapshot.enabled = source.enabled;
    snapshot.controllerPath = source.controllerPath;
    snapshot.clipSources = source.clipSources;
    snapshot.defaultStateName = source.defaultStateName;
    snapshot.states = source.states;
    snapshot.anyStateTransitions = source.anyStateTransitions;
    snapshot.parameters = source.parameters;
    snapshot.playing = source.playing;
    return snapshot;
}

// Undo適用時はランタイム資源を保持し、Graph定義だけを書き戻す。
void ApplyAnimationGraphSnapshot(
    scene::AnimatorComponent& target,
    const scene::AnimatorComponent& snapshot)
{
    target.enabled = snapshot.enabled;
    target.controllerPath = snapshot.controllerPath;
    target.clipSources = snapshot.clipSources;
    target.defaultStateName = snapshot.defaultStateName;
    target.states = snapshot.states;
    target.anyStateTransitions = snapshot.anyStateTransitions;
    target.parameters = snapshot.parameters;
    target.playing = snapshot.playing;
    target.currentStateName.clear();
    target.blendToState.clear();
    target.stateTime = 0.0f;
    target.blendWeight = 0.0f;
}

void PushAnimationGraphCommand(EditorContext& ctx,
                               const std::string& owner,
                               scene::EntityID entityId,
                               const std::shared_ptr<scene::AnimatorComponent>& assetAnimator,
                               const scene::AnimatorComponent& beforeAnimator,
                               const scene::AnimatorComponent& afterAnimator,
                               const GraphLayout& beforeLayout,
                               const GraphLayout& afterLayout)
{
    if (!ctx.undoStack) return;

    scene::Scene* scene = ctx.activeScene;
    std::string targetInstanceId;
    if (entityId.IsValid() && scene) {
        if (auto* go = scene->GetGameObject(entityId))
            targetInstanceId = go->instanceId;
    }
    EditorContext* context = &ctx;
    const auto markDirty = ctx.markSceneDirty;
    auto apply = [scene, context, owner, targetInstanceId, assetAnimator, markDirty](
                     const scene::AnimatorComponent& animator,
                     const GraphLayout& layout) {
        if (!targetInstanceId.empty() && scene) {
            if (auto* go = scene->FindByGuid(targetInstanceId)) {
                if (auto* target = go->GetComponent<scene::AnimatorComponent>())
                    ApplyAnimationGraphSnapshot(*target, animator);
            }
            if (markDirty) markDirty();
        } else if (assetAnimator) {
            ApplyAnimationGraphSnapshot(*assetAnimator, animator);
            context->animationControllerDirty = true;
        }
        context->graphLayouts[owner] = layout;
        context->animationGraphSelection.Clear();
    };

    ctx.undoStack->Push(std::make_unique<LambdaCommand>(
        "Edit Animation Graph",
        [apply, afterAnimator, afterLayout]() { apply(afterAnimator, afterLayout); },
        [apply, beforeAnimator, beforeLayout]() { apply(beforeAnimator, beforeLayout); }));
}

void TrackAnimationGraphUndo(EditorContext& ctx,
                             const std::string& owner,
                             scene::EntityID entityId,
                             const std::shared_ptr<scene::AnimatorComponent>& assetAnimator,
                             scene::AnimatorComponent& animator,
                             const scene::AnimatorComponent& beforeDraw,
                             const GraphLayout& beforeLayout,
                             std::uint64_t generationBefore)
{
    static AnimationGraphUndoTracker tracker;
    const ImGuiID activeId = ImGui::GetActiveID();
    const bool changed = AnimationGraphEditGeneration() != generationBefore;

    if (tracker.active && tracker.owner != owner) {
        tracker.active = false;
    } else if (tracker.active && activeId != tracker.activeId) {
        PushAnimationGraphCommand(
            ctx, tracker.owner, tracker.entityId, tracker.assetAnimator,
            tracker.beforeAnimator, MakeAnimationGraphSnapshot(animator),
            tracker.beforeLayout, ctx.graphLayouts[owner]);
        tracker.active = false;
    }

    if (!changed) return;
    if (activeId != 0) {
        if (!tracker.active) {
            tracker.owner = owner;
            tracker.activeId = activeId;
            tracker.entityId = entityId;
            tracker.assetAnimator = assetAnimator;
            tracker.beforeAnimator = beforeDraw;
            tracker.beforeLayout = beforeLayout;
            tracker.active = true;
        }
        return;
    }

    PushAnimationGraphCommand(
        ctx, owner, entityId, assetAnimator,
        beforeDraw, MakeAnimationGraphSnapshot(animator),
        beforeLayout, ctx.graphLayouts[owner]);
}

std::string MakeUniqueStateName(const scene::AnimatorComponent& animator, const char* baseName)
{
    const std::string base = (baseName && baseName[0] != '\0') ? baseName : "NewState";
    auto exists = [&](const std::string& name) {
        return std::any_of(animator.states.begin(), animator.states.end(),
            [&](const scene::AnimationState& state) { return state.name == name; });
    };

    if (!exists(base)) return base;
    for (int i = 1; i < 10000; ++i) {
        std::string candidate = base + std::to_string(i);
        if (!exists(candidate)) return candidate;
    }
    return base + "_";
}

int FindStateIndexByName(const scene::AnimatorComponent& animator, const std::string& name)
{
    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        if (animator.states[static_cast<size_t>(i)].name == name) return i;
    }
    return -1;
}

bool IsFloatCondition(scene::ConditionOp op)
{
    return static_cast<int>(op) <= static_cast<int>(scene::ConditionOp::NotEqual);
}

float ClampZoom(float zoom)
{
    return std::clamp(zoom, MIN_CANVAS_ZOOM, MAX_CANVAS_ZOOM);
}

const char* StateModeName(scene::AnimationStateMode mode)
{
    switch (mode) {
    case scene::AnimationStateMode::Clip:        return "CLIP";
    case scene::AnimationStateMode::BlendTree1D: return "BLEND TREE 1D";
    case scene::AnimationStateMode::BlendTree2D: return "BLEND TREE 2D";
    }
    return "UNKNOWN";
}

ImVec4 StateModeTextColor(scene::AnimationStateMode mode)
{
    switch (mode) {
    case scene::AnimationStateMode::Clip:        return ImVec4(0.52f, 0.72f, 1.00f, 1.0f);
    case scene::AnimationStateMode::BlendTree1D: return ImVec4(0.35f, 0.88f, 0.94f, 1.0f);
    case scene::AnimationStateMode::BlendTree2D: return ImVec4(0.78f, 0.58f, 1.00f, 1.0f);
    }
    return ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
}

const char* ConditionOpSymbol(scene::ConditionOp op)
{
    switch (op) {
    case scene::ConditionOp::Greater:  return ">";
    case scene::ConditionOp::Less:     return "<";
    case scene::ConditionOp::Equal:    return "==";
    case scene::ConditionOp::NotEqual: return "!=";
    case scene::ConditionOp::True:     return "is true";
    case scene::ConditionOp::False:    return "is false";
    }
    return "?";
}

std::string BuildTransitionBadge(const scene::AnimationTransition& transition)
{
    std::string result;
    if (transition.hasExitTime) {
        char exitBuffer[32]{};
        std::snprintf(
            exitBuffer,
            sizeof(exitBuffer),
            "Exit %.0f%%",
            transition.exitTime * 100.0f);
        result = exitBuffer;
    }

    if (!transition.conditions.empty()) {
        if (!result.empty()) result += "  |  ";
        const auto& condition = transition.conditions.front();
        result += condition.paramName;
        result += " ";
        result += ConditionOpSymbol(condition.op);
        if (IsFloatCondition(condition.op)) {
            char thresholdBuffer[32]{};
            std::snprintf(
                thresholdBuffer,
                sizeof(thresholdBuffer),
                " %.2f",
                condition.threshold);
            result += thresholdBuffer;
        }
        if (transition.conditions.size() > 1)
            result += " +" + std::to_string(transition.conditions.size() - 1);
    }

    return result.empty() ? "Immediate" : result;
}

unsigned int NodeBackgroundColor(scene::AnimationStateMode mode,
                                 bool isCurrent,
                                 bool isDefault)
{
    if (isCurrent) return IM_COL32(24, 72, 48, 255);
    if (isDefault) return IM_COL32(74, 57, 22, 255);
    if (mode == scene::AnimationStateMode::BlendTree1D)
        return IM_COL32(27, 44, 52, 255);
    if (mode == scene::AnimationStateMode::BlendTree2D)
        return IM_COL32(42, 34, 54, 255);
    return IM_COL32(34, 39, 47, 255);
}

unsigned int NodeTitleColor(scene::AnimationStateMode mode,
                            bool isCurrent,
                            bool isDefault)
{
    if (isCurrent) return IM_COL32(41, 138, 82, 255);
    if (isDefault) return IM_COL32(156, 112, 32, 255);
    if (mode == scene::AnimationStateMode::BlendTree1D)
        return IM_COL32(35, 91, 105, 255);
    if (mode == scene::AnimationStateMode::BlendTree2D)
        return IM_COL32(91, 57, 112, 255);
    return IM_COL32(54, 62, 72, 255);
}

unsigned int NodeOutlineColor(bool isCurrent, bool isDefault)
{
    if (isCurrent) return IM_COL32(78, 226, 136, 255);
    if (isDefault) return IM_COL32(237, 184, 67, 255);
    return IM_COL32(108, 118, 132, 255);
}

bool DrawFloatParameterCombo(EditorContext& ctx,
                             const char* label,
                             const scene::AnimatorComponent& animator,
                             std::string& parameterName)
{
    const char* preview = parameterName.empty() ? "<Select Float Parameter>" : parameterName.c_str();
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (const auto& parameter : animator.parameters) {
            if (parameter.type != scene::ParamType::Float) continue;
            const bool selected = parameter.name == parameterName;
            if (ImGui::Selectable(parameter.name.c_str(), selected)) {
                parameterName = parameter.name;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (changed) MarkDirty(ctx);
    return changed;
}

bool DrawAnimationSource(EditorContext& ctx,
                         const char* label,
                         scene::AnimatorComponent& animator,
                         std::string& sourcePath)
{
    char sourceBuffer[512]{};
    std::snprintf(
        sourceBuffer, sizeof(sourceBuffer), "%s", sourcePath.c_str());
    bool changed = false;
    if (ImGui::InputText(label, sourceBuffer, sizeof(sourceBuffer))) {
        sourcePath = NormalizeAssetPath(sourceBuffer);
        changed = true;
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            sourcePath =
                NormalizeAssetPath(static_cast<const char*>(payload->Data));
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }
    if (changed) {
        animator.clips.clear();
        animator.clipSourcePaths.clear();
        animator.clipsLoaded = false;
        MarkDirty(ctx);
    }
    ImGui::TextDisabled("Drop an animation .fzasset source here.");
    return changed;
}

// 指定された Source / Clip の実再生秒数を返し、Graph UI の Length 表示に使用する。
float GetClipLength(const scene::AnimatorComponent& animator,
                    const std::string& sourcePath,
                    const std::string& clipName,
                    int clipIndex)
{
    const asset::AnimationClip* fallback = nullptr;
    for (size_t i = 0; i < animator.clips.size(); ++i) {
        if (!sourcePath.empty() &&
            (i >= animator.clipSourcePaths.size() ||
             animator.clipSourcePaths[i] != sourcePath))
            continue;
        if (!fallback) fallback = &animator.clips[i];
        if (!clipName.empty() && animator.clips[i].name == clipName) {
            fallback = &animator.clips[i];
            break;
        }
    }
    if (!fallback && clipIndex >= 0 &&
        clipIndex < static_cast<int>(animator.clips.size()))
        fallback = &animator.clips[static_cast<size_t>(clipIndex)];
    if (!fallback) return 0.0f;
    const double ticksPerSecond =
        fallback->ticksPerSecond > 0.0 ? fallback->ticksPerSecond : 30.0;
    return static_cast<float>(fallback->durationTicks / ticksPerSecond);
}

// 単一 Clip ステートの Length を返す。BlendTree は実行時 Weight 依存のため 0 を返す。
float GetStateClipLength(const scene::AnimatorComponent& animator,
                         const scene::AnimationState* state)
{
    if (!state || state->mode != scene::AnimationStateMode::Clip) return 0.0f;
    return GetClipLength(
        animator, state->sourcePath, state->clipName, state->clipIndex);
}

// Unity の Transition Preview と同様に、遷移元・遷移先・ブレンド区間を時間軸で表示する。
// WHY: 数値だけでは Clip Length に対する Duration の大きさを判断しづらいため。
void DrawTransitionTimeline(const scene::AnimatorComponent& animator,
                            const scene::AnimationState* sourceState,
                            const scene::AnimationState* destinationState,
                            const scene::AnimationTransition& transition,
                            float sourceLength,
                            float destinationLength)
{
    const float displaySourceLength = sourceLength > 0.0f ? sourceLength : 1.0f;
    const float displayDestinationLength =
        destinationLength > 0.0f ? destinationLength : displaySourceLength;
    const float blendSeconds = transition.fixedDuration
        ? transition.transitionDuration
        : transition.transitionDuration * displaySourceLength;
    const float transitionStart = transition.hasExitTime
        ? transition.exitTime * displaySourceLength
        : (std::max)(displaySourceLength - blendSeconds, 0.0f);
    const float destinationStart = transitionStart;
    const float timelineLength = (std::max)(
        (std::max)(displaySourceLength, transitionStart + blendSeconds),
        destinationStart + displayDestinationLength);

    ImGui::SeparatorText("Transition Preview");
    const float width = (std::max)(ImGui::GetContentRegionAvail().x, 220.0f);
    constexpr float HEIGHT = 94.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##TransitionTimeline", ImVec2(width, HEIGHT));

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 max(origin.x + width, origin.y + HEIGHT);
    drawList->AddRectFilled(origin, max, IM_COL32(27, 30, 35, 255), 4.0f);
    drawList->AddRect(origin, max, IM_COL32(76, 83, 94, 255), 4.0f);

    constexpr float LABEL_WIDTH = 54.0f;
    constexpr float RIGHT_PADDING = 10.0f;
    const float trackLeft = origin.x + LABEL_WIDTH;
    const float trackWidth = (std::max)(width - LABEL_WIDTH - RIGHT_PADDING, 1.0f);
    const auto timeToX = [&](float seconds) {
        return trackLeft +
            std::clamp(seconds / (std::max)(timelineLength, 0.0001f), 0.0f, 1.0f) *
                trackWidth;
    };

    drawList->AddText(
        ImVec2(origin.x + 8.0f, origin.y + 20.0f),
        IM_COL32(155, 188, 235, 255), "Source");
    drawList->AddText(
        ImVec2(origin.x + 8.0f, origin.y + 54.0f),
        IM_COL32(242, 177, 96, 255), "Dest");

    const ImVec2 sourceMin(trackLeft, origin.y + 18.0f);
    const ImVec2 sourceMax(timeToX(displaySourceLength), origin.y + 38.0f);
    const ImVec2 destinationMin(timeToX(destinationStart), origin.y + 52.0f);
    const ImVec2 destinationMax(
        timeToX(destinationStart + displayDestinationLength), origin.y + 72.0f);
    drawList->AddRectFilled(
        sourceMin, sourceMax, IM_COL32(67, 122, 190, 255), 3.0f);
    drawList->AddRectFilled(
        destinationMin, destinationMax, IM_COL32(202, 126, 53, 255), 3.0f);
    if (sourceState && sourceMax.x - sourceMin.x > 48.0f)
        drawList->AddText(
            ImVec2(sourceMin.x + 6.0f, sourceMin.y + 2.0f),
            IM_COL32(235, 242, 252, 255), sourceState->name.c_str());
    if (destinationState && destinationMax.x - destinationMin.x > 48.0f)
        drawList->AddText(
            ImVec2(destinationMin.x + 6.0f, destinationMin.y + 2.0f),
            IM_COL32(255, 241, 220, 255), destinationState->name.c_str());

    const float blendStartX = timeToX(transitionStart);
    const float blendEndX = timeToX(transitionStart + blendSeconds);
    drawList->AddRectFilled(
        ImVec2(blendStartX, origin.y + 15.0f),
        ImVec2(blendEndX, origin.y + 75.0f),
        IM_COL32(230, 210, 105, 52));
    drawList->AddLine(
        ImVec2(blendStartX, origin.y + 13.0f),
        ImVec2(blendStartX, origin.y + 78.0f),
        IM_COL32(244, 214, 104, 255), 2.0f);
    drawList->AddLine(
        ImVec2(blendEndX, origin.y + 13.0f),
        ImVec2(blendEndX, origin.y + 78.0f),
        IM_COL32(244, 214, 104, 180), 1.0f);

    if (sourceState && destinationState &&
        animator.currentStateName == sourceState->name &&
        animator.blendToState == destinationState->name) {
        const float runtimeTime =
            transitionStart + blendSeconds * std::clamp(animator.blendWeight, 0.0f, 1.0f);
        const float runtimeX = timeToX(runtimeTime);
        drawList->AddLine(
            ImVec2(runtimeX, origin.y + 8.0f),
            ImVec2(runtimeX, origin.y + 82.0f),
            IM_COL32(245, 245, 245, 255), 2.0f);
    }

    char durationText[96]{};
    std::snprintf(
        durationText, sizeof(durationText),
        "Blend %.3f s  |  Timeline %.3f s", blendSeconds, timelineLength);
    drawList->AddText(
        ImVec2(trackLeft, origin.y + 78.0f),
        IM_COL32(166, 172, 182, 255), durationText);

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Blue: source clip\nOrange: destination clip\nYellow: transition blend");
    }
}

bool DrawClipCombo(EditorContext& ctx,
                   const char* label,
                   scene::AnimatorComponent& animator,
                   const std::string& sourcePath,
                   std::string& clipName,
                   int& clipIndex)
{
    const bool sourceAlreadyLoaded = std::any_of(
        animator.clipSourcePaths.begin(),
        animator.clipSourcePaths.end(),
        [&sourcePath](const std::string& loadedSource) {
            return loadedSource == sourcePath;
        });
    if (!sourcePath.empty() && !sourceAlreadyLoaded) {
        if (auto model = asset::AssetManager::Load<asset::Model>(sourcePath)) {
            for (const auto& clip : model->clips) {
                animator.clips.push_back(clip);
                animator.clipSourcePaths.push_back(sourcePath);
            }
        }
    }

    const char* preview = clipName.empty() ? "<Auto / First Clip>" : clipName.c_str();
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        if (ImGui::Selectable("<Auto / First Clip>", clipName.empty())) {
            clipName.clear();
            clipIndex = -1;
            changed = true;
        }
        for (int i = 0; i < static_cast<int>(animator.clips.size()); ++i) {
            if (!sourcePath.empty() &&
                (i >= static_cast<int>(animator.clipSourcePaths.size()) ||
                 animator.clipSourcePaths[static_cast<size_t>(i)] != sourcePath))
                continue;
            const auto& clip = animator.clips[static_cast<size_t>(i)];
            const bool selected = clipName == clip.name;
            if (ImGui::Selectable(clip.name.c_str(), selected)) {
                clipName = clip.name;
                clipIndex = i;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (changed) MarkDirty(ctx);
    const float clipLength =
        GetClipLength(animator, sourcePath, clipName, clipIndex);
    if (clipLength > 0.0f)
        ImGui::TextDisabled("Clip Length: %.3f s", clipLength);
    else
        ImGui::TextDisabled("Clip Length: unavailable");
    return changed;
}

bool HasFloatParameter(const scene::AnimatorComponent& animator, const std::string& name)
{
    return std::any_of(
        animator.parameters.begin(),
        animator.parameters.end(),
        [&name](const scene::AnimatorParameter& parameter) {
            return parameter.type == scene::ParamType::Float && parameter.name == name;
        });
}

void DrawBlendTreeWarnings(const scene::AnimatorComponent& animator,
                           const scene::AnimationState& state)
{
    const auto warning = [](const char* text) {
        ImGui::TextColored(ImVec4(1.0f, 0.68f, 0.25f, 1.0f), "Warning: %s", text);
    };

    if (state.mode == scene::AnimationStateMode::BlendTree1D) {
        if (!HasFloatParameter(animator, state.blendTree1D.paramName))
            warning("Select an existing Float parameter.");
        if (state.blendTree1D.motions.empty())
            warning("Add at least one motion.");
        for (size_t i = 0; i < state.blendTree1D.motions.size(); ++i) {
            const auto& motion = state.blendTree1D.motions[i];
            if (motion.sourcePath.empty())
                warning("A motion has no source.");
            for (size_t j = i + 1; j < state.blendTree1D.motions.size(); ++j) {
                if (std::abs(
                        motion.threshold -
                        state.blendTree1D.motions[j].threshold) <= 0.0001f) {
                    warning("Duplicate thresholds produce an abrupt selection.");
                    return;
                }
            }
        }
        return;
    }

    if (!HasFloatParameter(animator, state.blendTree2D.paramX) ||
        !HasFloatParameter(animator, state.blendTree2D.paramY))
        warning("Select existing Float parameters for both axes.");
    if (state.blendTree2D.paramX == state.blendTree2D.paramY &&
        !state.blendTree2D.paramX.empty())
        warning("X and Y use the same parameter.");
    if (state.blendTree2D.motions.empty())
        warning("Add at least one motion.");
    for (size_t i = 0; i < state.blendTree2D.motions.size(); ++i) {
        const auto& motion = state.blendTree2D.motions[i];
        if (motion.sourcePath.empty()) warning("A motion has no source.");
        for (size_t j = i + 1; j < state.blendTree2D.motions.size(); ++j) {
            const auto& other = state.blendTree2D.motions[j];
            if (std::abs(motion.posX - other.posX) <= 0.0001f &&
                std::abs(motion.posY - other.posY) <= 0.0001f) {
                warning("Duplicate 2D positions cannot be blended reliably.");
                return;
            }
        }
    }
}

void DrawBlendTreeEditor(EditorContext& ctx,
                         scene::AnimatorComponent& animator,
                         scene::AnimationState& state)
{
    static constexpr const char* MODE_NAMES[] = { "Clip", "Blend Tree 1D", "Blend Tree 2D" };
    int mode = static_cast<int>(state.mode);
    if (ImGui::Combo("Mode", &mode, MODE_NAMES, 3)) {
        state.mode = static_cast<scene::AnimationStateMode>(mode);
        MarkDirty(ctx);
    }

    if (state.mode == scene::AnimationStateMode::Clip) {
        DrawAnimationSource(ctx, "Source", animator, state.sourcePath);
        DrawClipCombo(
            ctx,
            "Clip",
            animator,
            state.sourcePath,
            state.clipName,
            state.clipIndex);
        return;
    }

    DrawBlendTreeWarnings(animator, state);

    auto drawMotion = [&](scene::BlendTreeMotion& motion, bool is2D) {
        DrawAnimationSource(ctx, "Source", animator, motion.sourcePath);
        DrawClipCombo(
            ctx,
            "Clip",
            animator,
            motion.sourcePath,
            motion.clipName,
            motion.clipIndex);
        if (is2D) {
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::DragFloat("X", &motion.posX, 0.01f)) MarkDirty(ctx);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::DragFloat("Y", &motion.posY, 0.01f)) MarkDirty(ctx);
        } else if (ImGui::DragFloat("Threshold", &motion.threshold, 0.01f)) {
            MarkDirty(ctx);
        }
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::DragFloat("Motion Speed", &motion.speed, 0.01f, -10.0f, 10.0f))
            MarkDirty(ctx);
        if (ImGui::DragFloat("Motion IK", &motion.ikWeight, 0.01f, 0.0f, 1.0f))
            MarkDirty(ctx);
    };

    if (state.mode == scene::AnimationStateMode::BlendTree1D) {
        DrawFloatParameterCombo(ctx, "Parameter", animator, state.blendTree1D.paramName);
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::DragFloat(
                "Damp Time", &state.blendTree1D.dampTime,
                0.01f, 0.0f, 2.0f, "%.2f s"))
            MarkDirty(ctx);
        ImGui::TextDisabled(
            "Runtime: Raw %.3f  |  Blended %.3f",
            animator.GetFloat(state.blendTree1D.paramName),
            state.blendTree1D.dampedValueInitialized
                ? state.blendTree1D.dampedValue
                : animator.GetFloat(state.blendTree1D.paramName));
        int removeIndex = -1;
        int duplicateIndex = -1;
        for (int i = 0; i < static_cast<int>(state.blendTree1D.motions.size()); ++i) {
            ImGui::PushID(i);
            auto& motion = state.blendTree1D.motions[static_cast<size_t>(i)];
            char header[160]{};
            std::snprintf(
                header, sizeof(header), "Motion %d  |  %.3f  |  %s",
                i + 1, motion.threshold,
                motion.clipName.empty() ? "<No Clip>" : motion.clipName.c_str());
            const bool open = ImGui::CollapsingHeader(
                header, ImGuiTreeNodeFlags_DefaultOpen);
            if (open) drawMotion(motion, false);
            if (open && ImGui::SmallButton("Duplicate")) duplicateIndex = i;
            if (open) ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) removeIndex = i;
            ImGui::PopID();
        }
        if (duplicateIndex >= 0) {
            auto copy = state.blendTree1D.motions[static_cast<size_t>(duplicateIndex)];
            copy.threshold += 0.1f;
            state.blendTree1D.motions.insert(
                state.blendTree1D.motions.begin() + duplicateIndex + 1, copy);
            MarkDirty(ctx);
        }
        if (removeIndex >= 0) {
            state.blendTree1D.motions.erase(
                state.blendTree1D.motions.begin() + removeIndex);
            MarkDirty(ctx);
        }
        if (ImGui::Button("+ Motion")) {
            scene::BlendTreeMotion motion;
            if (!animator.clips.empty()) {
                motion.clipName = animator.clips.front().name;
                motion.clipIndex = 0;
            }
            if (!state.blendTree1D.motions.empty())
                motion.threshold = state.blendTree1D.motions.back().threshold + 1.0f;
            state.blendTree1D.motions.push_back(std::move(motion));
            MarkDirty(ctx);
        }
        ImGui::SameLine();
        if (ImGui::Button("Sort Thresholds")) {
            std::sort(state.blendTree1D.motions.begin(), state.blendTree1D.motions.end(),
                [](const auto& a, const auto& b) { return a.threshold < b.threshold; });
            MarkDirty(ctx);
        }
    } else {
        DrawFloatParameterCombo(ctx, "Parameter X", animator, state.blendTree2D.paramX);
        DrawFloatParameterCombo(ctx, "Parameter Y", animator, state.blendTree2D.paramY);
        static constexpr const char* TYPE_NAMES[] = {
            "Simple Directional", "Freeform Cartesian"
        };
        int type = static_cast<int>(state.blendTree2D.type);
        if (ImGui::Combo("2D Type", &type, TYPE_NAMES, 2)) {
            state.blendTree2D.type = static_cast<scene::BlendTree2DType>(type);
            MarkDirty(ctx);
        }

        const ImVec2 previewSize(
            std::clamp(ImGui::GetContentRegionAvail().x, 220.0f, 420.0f),
            220.0f);
        const ImVec2 previewMin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##BlendTree2DPreview", previewSize);
        const ImVec2 previewEnd = ImGui::GetCursorScreenPos();
        const ImVec2 previewMax(
            previewMin.x + previewSize.x, previewMin.y + previewSize.y);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(previewMin, previewMax, IM_COL32(24, 27, 31, 255));
        drawList->AddRect(previewMin, previewMax, IM_COL32(95, 105, 118, 255));
        const ImVec2 center(
            (previewMin.x + previewMax.x) * 0.5f,
            (previewMin.y + previewMax.y) * 0.5f);
        drawList->AddLine(
            ImVec2(previewMin.x, center.y), ImVec2(previewMax.x, center.y),
            IM_COL32(65, 72, 82, 255));
        drawList->AddLine(
            ImVec2(center.x, previewMin.y), ImVec2(center.x, previewMax.y),
            IM_COL32(65, 72, 82, 255));

        float range = 1.0f;
        for (const auto& motion : state.blendTree2D.motions)
            range = (std::max)(
                range, (std::max)(std::abs(motion.posX), std::abs(motion.posY)));
        const float scale = ((std::min)(previewSize.x, previewSize.y) * 0.42f) / range;
        for (int i = 0; i < static_cast<int>(state.blendTree2D.motions.size()); ++i) {
            auto& motion = state.blendTree2D.motions[static_cast<size_t>(i)];
            const ImVec2 point(
                center.x + motion.posX * scale,
                center.y - motion.posY * scale);
            ImGui::PushID(i);
            ImGui::SetCursorScreenPos(ImVec2(point.x - 8.0f, point.y - 8.0f));
            ImGui::InvisibleButton("##MotionPoint", ImVec2(16.0f, 16.0f));
            const bool hovered = ImGui::IsItemHovered();
            const bool active = ImGui::IsItemActive();
            if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                motion.posX = (mouse.x - center.x) / scale;
                motion.posY = (center.y - mouse.y) / scale;
                MarkDirty(ctx);
            }
            drawList->AddCircleFilled(
                point, active ? 7.0f : 5.0f,
                hovered ? IM_COL32(255, 218, 132, 255)
                        : IM_COL32(255, 174, 72, 255));
            if (hovered)
                ImGui::SetTooltip(
                    "%s\nX %.3f  Y %.3f\nDrag to move",
                    motion.clipName.empty() ? "<No Clip>" : motion.clipName.c_str(),
                    motion.posX, motion.posY);
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(previewEnd);
        const ImVec2 runtimePoint(
            center.x + animator.GetFloat(state.blendTree2D.paramX) * scale,
            center.y - animator.GetFloat(state.blendTree2D.paramY) * scale);
        drawList->AddCircle(
            runtimePoint, 7.0f, IM_COL32(78, 208, 255, 255), 0, 2.0f);
        ImGui::TextDisabled(
            "Runtime: X %.3f  Y %.3f | Drag orange points to edit",
            animator.GetFloat(state.blendTree2D.paramX),
            animator.GetFloat(state.blendTree2D.paramY));

        int removeIndex = -1;
        int duplicateIndex = -1;
        for (int i = 0; i < static_cast<int>(state.blendTree2D.motions.size()); ++i) {
            ImGui::PushID(i);
            auto& motion = state.blendTree2D.motions[static_cast<size_t>(i)];
            char header[192]{};
            std::snprintf(
                header, sizeof(header), "Motion %d  |  (%.2f, %.2f)  |  %s",
                i + 1, motion.posX, motion.posY,
                motion.clipName.empty() ? "<No Clip>" : motion.clipName.c_str());
            const bool open = ImGui::CollapsingHeader(
                header, ImGuiTreeNodeFlags_DefaultOpen);
            if (open) drawMotion(motion, true);
            if (open && ImGui::SmallButton("Duplicate")) duplicateIndex = i;
            if (open) ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) removeIndex = i;
            ImGui::PopID();
        }
        if (duplicateIndex >= 0) {
            auto copy = state.blendTree2D.motions[static_cast<size_t>(duplicateIndex)];
            copy.posX += 0.1f;
            copy.posY += 0.1f;
            state.blendTree2D.motions.insert(
                state.blendTree2D.motions.begin() + duplicateIndex + 1, copy);
            MarkDirty(ctx);
        }
        if (removeIndex >= 0) {
            state.blendTree2D.motions.erase(
                state.blendTree2D.motions.begin() + removeIndex);
            MarkDirty(ctx);
        }
        if (ImGui::Button("+ Motion")) {
            scene::BlendTreeMotion motion;
            if (!animator.clips.empty()) {
                motion.clipName = animator.clips.front().name;
                motion.clipIndex = 0;
            }
            motion.posX = static_cast<float>(state.blendTree2D.motions.size());
            state.blendTree2D.motions.push_back(std::move(motion));
            MarkDirty(ctx);
        }
    }

    if (!animator.currentBlendWeights.empty()) {
        ImGui::SeparatorText("Runtime Weights");
        for (const auto& [clipName, weight] : animator.currentBlendWeights) {
            char overlay[192]{};
            std::snprintf(
                overlay, sizeof(overlay), "%s  %.1f%%",
                clipName.c_str(), weight * 100.0f);
            ImGui::ProgressBar(weight, ImVec2(-1.0f, 0.0f), overlay);
        }
    }
}

} // namespace

static void DrawTransitionEditor(EditorContext& ctx,
                                 scene::AnimatorComponent& animator,
                                 const scene::AnimationState* sourceState,
                                 scene::AnimationTransition& transition);

void AnimationGraphPanel::OnInit(EditorContext&)
{
    ImNodes::SetImGuiContext(ImGui::GetCurrentContext());
    m_nodesContext = ImNodes::CreateContext();
    ImNodes::SetCurrentContext(m_nodesContext);
    ImNodes::StyleColorsDark();
    auto& style = ImNodes::GetStyle();
    style.Flags |= ImNodesStyleFlags_GridLinesPrimary;
    style.Colors[ImNodesCol_GridBackground] = IM_COL32(22, 25, 30, 255);
    style.Colors[ImNodesCol_GridLine] = IM_COL32(45, 51, 60, 120);
    style.Colors[ImNodesCol_GridLinePrimary] = IM_COL32(61, 69, 80, 165);
    style.Colors[ImNodesCol_MiniMapBackground] = IM_COL32(18, 21, 26, 225);
    style.Colors[ImNodesCol_MiniMapOutline] = IM_COL32(91, 103, 119, 220);
    m_editorContext = ImNodes::EditorContextCreate();
}

void AnimationGraphPanel::OnShutdown()
{
    if (m_nodesContext) ImNodes::SetCurrentContext(m_nodesContext);
    if (m_editorContext) {
        ImNodes::EditorContextFree(m_editorContext);
        m_editorContext = nullptr;
    }
    if (m_nodesContext) {
        ImNodes::DestroyContext(m_nodesContext);
        m_nodesContext = nullptr;
    }
}

int AnimationGraphPanel::NodeId(int stateIndex)
{
    return stateIndex + 1;
}

int AnimationGraphPanel::InputPinId(int stateIndex)
{
    return stateIndex * 2 + 1;
}

int AnimationGraphPanel::OutputPinId(int stateIndex)
{
    return stateIndex * 2 + 2;
}

int AnimationGraphPanel::LinkId(int fromStateIndex, int transitionIndex)
{
    return (fromStateIndex << 16) | transitionIndex;
}

int AnimationGraphPanel::AnyStateNodeId() { return 1000000; }
int AnimationGraphPanel::AnyStateOutputPinId() { return 1000002; }
int AnimationGraphPanel::AnyStateLinkId(int transitionIndex)
{
    return 0x70000000 | transitionIndex;
}
int AnimationGraphPanel::EntryNodeId() { return 1000010; }
int AnimationGraphPanel::EntryOutputPinId() { return 1000012; }
int AnimationGraphPanel::EntryLinkId() { return 0x60000000; }

void AnimationGraphPanel::OnRenderContent(EditorContext& ctx)
{
    FBZZ_PROFILE_SCOPE("AnimationGraphPanel::Render");

    // WHY: Play Mode 中は Animator の実行時状態が毎フレーム変化する。
    //      編集用 snapshot と Undo 追跡を続けると、監視表示だけで大きな CPU 負荷になる。
    const bool allowEditing = CanEditAnimationGraph(ctx);
    const bool editingControllerAsset =
        util::StringUtils::EndsWith(ctx.selectedAssetPath, ".fbzzanimcontroller");
    if (editingControllerAsset) {
        if (ctx.animationControllerEditorPath != ctx.selectedAssetPath ||
            !ctx.animationControllerEditor) {
            asset::AnimatorControllerAsset controller;
            if (!asset::LoadAnimatorControllerAsset(ctx.selectedAssetPath, controller)) {
                ImGui::TextDisabled("Failed to load Animator Controller.");
                return;
            }
            ctx.animationControllerEditor =
                std::make_shared<scene::AnimatorComponent>();
            asset::ApplyAnimatorControllerAsset(
                controller, *ctx.animationControllerEditor);
            ctx.animationControllerEditorPath = ctx.selectedAssetPath;
            ctx.animationControllerDirty = false;
            std::unordered_map<std::string, GraphLayout> controllerLayouts;
            if (GraphLayoutSerializer::Load(
                    controllerLayouts, ctx.selectedAssetPath)) {
                if (const auto it = controllerLayouts.find(ctx.selectedAssetPath);
                    it != controllerLayouts.end()) {
                    ctx.graphLayouts[ctx.selectedAssetPath] = it->second;
                }
            }
            m_selectionOwnerInstanceId.clear();
            m_selectedLink = {};
            m_selectedNode = -1;
            m_openBlendTreeState = -1;
            m_selectedMotion = -1;
            m_selectedAnyState = false;
            ctx.animationGraphSelection.Clear();
        }

        auto& animator = *ctx.animationControllerEditor;
        scene::AnimatorComponent undoBeforeAnimator;
        GraphLayout undoBeforeLayout;
        if (allowEditing) {
            undoBeforeAnimator = MakeAnimationGraphSnapshot(animator);
            undoBeforeLayout = ctx.graphLayouts[ctx.selectedAssetPath];
        }
        const std::uint64_t undoGenerationBefore = AnimationGraphEditGeneration();
        ClearInvalidSelection(animator);
        ImGui::BeginDisabled(!allowEditing);
        ImGui::TextUnformatted(
            util::FileSystem::GetFilename(ctx.selectedAssetPath).c_str());
        ImGui::SameLine();
        if (m_openBlendTreeState < 0 && ImGui::Button("+ State"))
            AddState(ctx, animator, "NewState");
        if (m_openBlendTreeState >= 0 &&
            m_openBlendTreeState < static_cast<int>(animator.states.size())) {
            const std::string breadcrumbName =
                animator.states[static_cast<size_t>(m_openBlendTreeState)].name;
            ImGui::SameLine();
            if (ImGui::SmallButton("Base Layer")) {
                m_openBlendTreeState = -1;
                m_selectedMotion = -1;
                ImNodes::ClearNodeSelection();
                ImNodes::EditorContextResetPanning(ImVec2(0.0f, 0.0f));
            }
            ImGui::SameLine();
            ImGui::TextDisabled("> %s", breadcrumbName.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Save Controller")) {
            const auto controller = asset::MakeAnimatorControllerAsset(animator);
            if (asset::SaveAnimatorControllerAsset(
                    ctx.selectedAssetPath, controller)) {
                ctx.animationControllerDirty = false;
                AssetDirtyRegistry::MarkClean(ctx.selectedAssetPath);
                ctx.requestAssetBrowserRefresh = true;
                std::unordered_map<std::string, GraphLayout> controllerLayouts;
                controllerLayouts.emplace(
                    ctx.selectedAssetPath,
                    ctx.graphLayouts[ctx.selectedAssetPath]);
                GraphLayoutSerializer::Save(
                    controllerLayouts, ctx.selectedAssetPath);
                if (ctx.activeScene) {
                    const std::string savedPath =
                        NormalizeAssetPath(ctx.selectedAssetPath);
                    for (auto [sceneAnimator] :
                         ctx.activeScene->View<scene::AnimatorComponent>()) {
                        if (NormalizeAssetPath(sceneAnimator.controllerPath) == savedPath) {
                            asset::ApplyAnimatorControllerAsset(
                                controller, sceneAnimator);
                            sceneAnimator.loadedControllerPath =
                                sceneAnimator.controllerPath;
                        }
                    }
                }
            }
        }
        DrawZoomControls();
        ImGui::Separator();
        ImGui::BeginChild("##AnimationGraphRoot", ImVec2(0.0f, 0.0f), false);
        DrawParameterSidebar(ctx, animator);
        ImGui::SameLine();
        ImGui::BeginGroup();
        {
            FBZZ_PROFILE_SCOPE("AnimationGraphPanel::Canvas");
            if (m_openBlendTreeState >= 0)
                DrawBlendTreeCanvas(ctx, animator, ctx.selectedAssetPath);
            else
                DrawNodeCanvas(ctx, animator, ctx.selectedAssetPath);
        }
        ImGui::EndGroup();
        ImGui::EndChild();
        ImGui::EndDisabled();

        auto& selection = ctx.animationGraphSelection;
        selection.Clear();
        selection.assetPath = ctx.selectedAssetPath;
        if (m_selectedLink.fromStateIndex == -2) {
            selection.type = EditorContext::AnimationGraphSelection::Type::AnyStateTransition;
            selection.transitionIndex = m_selectedLink.transitionIndex;
        } else if (m_selectedLink.fromStateIndex >= 0) {
            selection.type = EditorContext::AnimationGraphSelection::Type::Transition;
            selection.stateIndex = m_selectedLink.fromStateIndex;
            selection.transitionIndex = m_selectedLink.transitionIndex;
        } else if (m_selectedNode >= 0) {
            selection.type = EditorContext::AnimationGraphSelection::Type::State;
            selection.stateIndex = m_selectedNode;
        } else if (m_selectedAnyState) {
            selection.type = EditorContext::AnimationGraphSelection::Type::AnyState;
        }
        if (allowEditing) {
            TrackAnimationGraphUndo(
                ctx,
                ctx.selectedAssetPath,
                {},
                ctx.animationControllerEditor,
                animator,
                undoBeforeAnimator,
                undoBeforeLayout,
                undoGenerationBefore);
        }
        return;
    }

    scene::GameObject* go = ctx.GetSelectedGO();
    if (!go) {
        ImGui::TextDisabled("Select a GameObject with AnimatorComponent.");
        return;
    }

    auto* animator = go->GetComponent<scene::AnimatorComponent>();
    if (!animator) {
        ImGui::TextDisabled("Selected GameObject has no AnimatorComponent.");
        return;
    }

    if (m_selectionOwnerInstanceId != go->instanceId) {
        m_selectionOwnerInstanceId = go->instanceId;
        m_selectedLink = {};
        m_selectedNode = -1;
        m_openBlendTreeState = -1;
        m_selectedMotion = -1;
        m_selectedAnyState = false;
        ctx.animationGraphSelection.Clear();
    }

    ClearInvalidSelection(*animator);
    scene::AnimatorComponent undoBeforeAnimator;
    GraphLayout undoBeforeLayout;
    if (allowEditing) {
        undoBeforeAnimator = MakeAnimationGraphSnapshot(*animator);
        undoBeforeLayout = ctx.graphLayouts[go->instanceId];
    }
    const std::uint64_t undoGenerationBefore = AnimationGraphEditGeneration();
    ImGui::BeginDisabled(!allowEditing);
    DrawToolbar(ctx, *animator);
    if (m_openBlendTreeState >= 0 &&
        m_openBlendTreeState < static_cast<int>(animator->states.size())) {
        const std::string breadcrumbName =
            animator->states[static_cast<size_t>(m_openBlendTreeState)].name;
        ImGui::SameLine();
        if (ImGui::SmallButton("Base Layer")) {
            m_openBlendTreeState = -1;
            m_selectedMotion = -1;
            ImNodes::ClearNodeSelection();
            ImNodes::EditorContextResetPanning(ImVec2(0.0f, 0.0f));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("> %s", breadcrumbName.c_str());
    }
    ImGui::Separator();

    ImGui::BeginChild("##AnimationGraphRoot", ImVec2(0.0f, 0.0f), false);
    DrawParameterSidebar(ctx, *animator);
    ImGui::SameLine();
    ImGui::BeginGroup();
    {
        FBZZ_PROFILE_SCOPE("AnimationGraphPanel::Canvas");
        if (m_openBlendTreeState >= 0)
            DrawBlendTreeCanvas(ctx, *animator, go->instanceId);
        else
            DrawNodeCanvas(ctx, *animator, go->instanceId);
    }
    ImGui::EndGroup();
    ImGui::EndChild();
    ImGui::EndDisabled();
    PublishSelection(ctx, *go);
    if (allowEditing) {
        TrackAnimationGraphUndo(
            ctx,
            go->instanceId,
            go->GetID(),
            {},
            *animator,
            undoBeforeAnimator,
            undoBeforeLayout,
            undoGenerationBefore);
    }
}

void AnimationGraphPanel::DrawToolbar(EditorContext& ctx, scene::AnimatorComponent& animator)
{
    scene::GameObject* go = ctx.GetSelectedGO();
    ImGui::TextUnformatted(go ? go->name.c_str() : "Animation Graph");
    ImGui::SameLine();

    if (m_openBlendTreeState < 0 && ImGui::Button("+ State")) {
        AddState(ctx, animator, "NewState");
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Param")) {
        scene::AnimatorParameter param;
        param.name = "NewParam";
        param.type = scene::ParamType::Float;
        animator.parameters.push_back(std::move(param));
        MarkDirty(ctx);
    }
    ImGui::SameLine();
    if (ImGui::Button(animator.playing ? "Pause" : "Play")) {
        animator.playing = !animator.playing;
        MarkDirty(ctx);
    }

    if (!animator.currentStateName.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Current: %s", animator.currentStateName.c_str());
    }
    if (!animator.blendToState.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("-> %s %.0f%%", animator.blendToState.c_str(), animator.blendWeight * 100.0f);
    }

    DrawZoomControls();
}

void AnimationGraphPanel::DrawZoomControls()
{
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::SmallButton("-##ZoomOut")) {
        m_canvasZoom = ClampZoom(m_canvasZoom - ZOOM_STEP);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("1:1##ZoomReset")) {
        m_canvasZoom = 1.0f;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+##ZoomIn")) {
        m_canvasZoom = ClampZoom(m_canvasZoom + ZOOM_STEP);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d%%", static_cast<int>(std::round(m_canvasZoom * 100.0f)));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Wheel: Zoom | Shift + Wheel: Horizontal pan | Alt + Wheel: Vertical pan");
}

void AnimationGraphPanel::DrawParameterSidebar(EditorContext& ctx, scene::AnimatorComponent& animator)
{
    ImGui::BeginChild("##AnimationGraphParameters", ImVec2(SIDEBAR_WIDTH, 0.0f), true);
    ImGui::TextUnformatted("Parameters");
    ImGui::Separator();

    static constexpr const char* PARAM_TYPES[] = { "Float", "Int", "Bool", "Trigger" };
    int removeIndex = -1;
    for (int i = 0; i < static_cast<int>(animator.parameters.size()); ++i) {
        auto& param = animator.parameters[static_cast<size_t>(i)];
        ImGui::PushID(i);
        ImGui::BeginGroup();

        int typeIndex = static_cast<int>(param.type);
        ImGui::SetNextItemWidth(86.0f);
        if (ImGui::Combo("##Type", &typeIndex, PARAM_TYPES, 4)) {
            param.type = static_cast<scene::ParamType>(typeIndex);
            MarkDirty(ctx);
        }
        ImGui::SameLine();

        char nameBuffer[96]{};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", param.name.c_str());
        ImGui::SetNextItemWidth(112.0f);
        if (ImGui::InputText("##Name", nameBuffer, sizeof(nameBuffer))) {
            param.name = nameBuffer;
            MarkDirty(ctx);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeIndex = i;

        switch (param.type) {
        case scene::ParamType::Float:
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragFloat("##Value", &param.floatValue, 0.01f)) MarkDirty(ctx);
            break;
        case scene::ParamType::Int:
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragInt("##Value", &param.intValue)) MarkDirty(ctx);
            break;
        case scene::ParamType::Bool:
            if (ImGui::Checkbox("Value", &param.boolValue)) MarkDirty(ctx);
            break;
        case scene::ParamType::Trigger:
            if (ImGui::SmallButton(param.boolValue ? "Triggered" : "Fire")) {
                param.boolValue = true;
                MarkDirty(ctx);
            }
            break;
        }

        ImGui::TextDisabled("%s", ParamTypeName(param.type));
        ImGui::EndGroup();
        ImGui::Separator();
        ImGui::PopID();
    }

    if (removeIndex >= 0) {
        animator.parameters.erase(animator.parameters.begin() + removeIndex);
        MarkDirty(ctx);
    }

    if (ImGui::Button("+ Add Parameter", ImVec2(-1.0f, 0.0f))) {
        scene::AnimatorParameter param;
        param.name = "NewParam";
        animator.parameters.push_back(std::move(param));
        MarkDirty(ctx);
    }
    ImGui::EndChild();
}

void AnimationGraphPanel::DrawNodeCanvas(EditorContext& ctx,
                                         scene::AnimatorComponent& animator,
                                         const std::string& instanceId)
{
    const float canvasHeight = std::max(220.0f, ImGui::GetContentRegionAvail().y);
    ImGui::BeginChild("##AnimationGraphCanvas", ImVec2(0.0f, 0.0f), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetWindowFontScale(m_canvasZoom);

    if (m_nodesContext) ImNodes::SetCurrentContext(m_nodesContext);
    if (m_editorContext) ImNodes::EditorContextSet(m_editorContext);

    const ImGuiIO& io = ImGui::GetIO();
    const bool canvasHovered = ImGui::IsWindowHovered(CANVAS_HOVER_FLAGS);
    const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
    const bool hasVerticalWheel = io.MouseWheel != 0.0f;
    const bool canZoomWithWheel =
        canvasHovered && !io.KeyShift && !io.KeyAlt && hasVerticalWheel;
    if (canZoomWithWheel) {
        // WHY: imnodes 本体にはズーム API がないため、パネル側でノード座標と描画寸法を拡縮する。
        //      カーソル直下のグラフ座標を固定するようパン量も補正し、拡大時の視点移動を防ぐ。
        const float oldZoom = m_canvasZoom;
        const float zoomSpeed = io.KeyCtrl ? ZOOM_STEP * 1.5f : ZOOM_STEP;
        const float newZoom = ClampZoom(oldZoom + io.MouseWheel * zoomSpeed);
        if (std::abs(newZoom - oldZoom) > 0.0001f) {
            const ImVec2 oldPanning = ImNodes::EditorContextGetPanning();
            const ImVec2 mouseInCanvas(
                io.MousePos.x - canvasOrigin.x,
                io.MousePos.y - canvasOrigin.y);
            const float ratio = newZoom / oldZoom;
            const ImVec2 newPanning(
                mouseInCanvas.x - (mouseInCanvas.x - oldPanning.x) * ratio,
                mouseInCanvas.y - (mouseInCanvas.y - oldPanning.y) * ratio);
            ImNodes::EditorContextResetPanning(newPanning);
            m_canvasZoom = newZoom;
        }
    }

    if (canvasHovered && (io.KeyShift || io.MouseWheelH != 0.0f)) {
        // Shift+縦ホイールと横ホイールを同じ横パン操作として扱う。
        const float horizontalWheel =
            io.MouseWheelH != 0.0f ? io.MouseWheelH : io.MouseWheel;
        if (horizontalWheel != 0.0f) {
            ImVec2 panning = ImNodes::EditorContextGetPanning();
            panning.x += horizontalWheel * WHEEL_PAN_STEP;
            ImNodes::EditorContextResetPanning(panning);
        }
    }
    if (canvasHovered && io.KeyAlt && !io.KeyShift && hasVerticalWheel) {
        ImVec2 panning = ImNodes::EditorContextGetPanning();
        panning.y += io.MouseWheel * WHEEL_PAN_STEP;
        ImNodes::EditorContextResetPanning(panning);
    }

    GraphLayout& layout = ctx.graphLayouts[instanceId];

    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        const auto& state = animator.states[static_cast<size_t>(i)];
        if (!layout.nodePositions.contains(state.name)) {
            layout.nodePositions[state.name] = ImVec2(80.0f + 260.0f * static_cast<float>(i), 80.0f);
        }
        const ImVec2 logicalPos = layout.nodePositions[state.name];
        ImNodes::SetNodeGridSpacePos(NodeId(i), ImVec2(logicalPos.x * m_canvasZoom, logicalPos.y * m_canvasZoom));
    }
    ImNodes::SetNodeGridSpacePos(
        EntryNodeId(),
        ImVec2(layout.entryPosition.x * m_canvasZoom, layout.entryPosition.y * m_canvasZoom));
    ImNodes::SetNodeGridSpacePos(
        AnyStateNodeId(),
        ImVec2(layout.anyStatePosition.x * m_canvasZoom, layout.anyStatePosition.y * m_canvasZoom));

    ImNodes::PushStyleVar(ImNodesStyleVar_GridSpacing, 32.0f * m_canvasZoom);
    ImNodes::PushStyleVar(ImNodesStyleVar_NodePadding, ImVec2(12.0f * m_canvasZoom, 8.0f * m_canvasZoom));
    ImNodes::PushStyleVar(ImNodesStyleVar_NodeCornerRounding, 6.0f * m_canvasZoom);
    ImNodes::PushStyleVar(ImNodesStyleVar_NodeBorderThickness, 2.0f * m_canvasZoom);
    ImNodes::PushStyleVar(ImNodesStyleVar_LinkThickness, 3.0f * m_canvasZoom);
    ImNodes::PushStyleVar(ImNodesStyleVar_PinCircleRadius, 5.0f * m_canvasZoom);
    ImNodes::PushStyleVar(ImNodesStyleVar_PinHoverRadius, 9.0f * m_canvasZoom);

    std::unordered_map<int, float> outputPinScreenY;
    std::unordered_map<int, float> inputPinScreenY;

    ImNodes::BeginNodeEditor();
    ImGui::SetWindowFontScale(m_canvasZoom);

    // Entry は Animator の初期化先を表す読み取り専用ノード。
    // WHAT: defaultStateName、未指定時は先頭 State へリンクして開始経路を可視化する。
    ImNodes::PushColorStyle(ImNodesCol_NodeBackground, IM_COL32(35, 82, 55, 255));
    ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundHovered, IM_COL32(42, 104, 68, 255));
    ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundSelected, IM_COL32(48, 120, 76, 255));
    ImNodes::PushColorStyle(ImNodesCol_NodeOutline, IM_COL32(82, 204, 122, 255));
    ImNodes::PushColorStyle(ImNodesCol_TitleBar, IM_COL32(50, 145, 82, 255));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, IM_COL32(60, 170, 96, 255));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, IM_COL32(66, 188, 106, 255));
    ImNodes::BeginNode(EntryNodeId());
    ImNodes::BeginNodeTitleBar();
    ImGui::TextUnformatted("Entry");
    ImNodes::EndNodeTitleBar();
    ImNodes::BeginStaticAttribute(EntryNodeId() + 10);
    const char* entryTargetName = "<none>";
    if (!animator.defaultStateName.empty())
        entryTargetName = animator.defaultStateName.c_str();
    else if (!animator.states.empty())
        entryTargetName = animator.states.front().name.c_str();
    ImGui::TextDisabled("DEFAULT FLOW");
    ImGui::Text("Target: %s", entryTargetName);
    ImNodes::EndStaticAttribute();
    ImNodes::BeginOutputAttribute(EntryOutputPinId());
    ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.62f, 1.0f), "START >");
    ImNodes::EndOutputAttribute();
    ImNodes::EndNode();
    for (int i = 0; i < 7; ++i) ImNodes::PopColorStyle();

    ImNodes::PushColorStyle(ImNodesCol_NodeBackground, IM_COL32(75, 45, 85, 255));
    ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundHovered, IM_COL32(96, 52, 110, 255));
    ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundSelected, IM_COL32(111, 58, 128, 255));
    ImNodes::PushColorStyle(ImNodesCol_NodeOutline, IM_COL32(190, 105, 222, 255));
    ImNodes::PushColorStyle(ImNodesCol_TitleBar, IM_COL32(125, 65, 145, 255));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, IM_COL32(151, 77, 174, 255));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, IM_COL32(169, 87, 194, 255));
    ImNodes::BeginNode(AnyStateNodeId());
    ImNodes::BeginNodeTitleBar();
    ImGui::TextUnformatted("Any State");
    ImNodes::EndNodeTitleBar();
    ImNodes::BeginStaticAttribute(AnyStateNodeId() + 10);
    ImGui::TextDisabled("GLOBAL TRANSITIONS");
    ImGui::Text("%d transition%s",
        static_cast<int>(animator.anyStateTransitions.size()),
        animator.anyStateTransitions.size() == 1 ? "" : "s");
    ImNodes::EndStaticAttribute();
    ImNodes::BeginOutputAttribute(AnyStateOutputPinId());
    outputPinScreenY[AnyStateNodeId()] = ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.5f;
    ImGui::TextColored(ImVec4(0.9f, 0.55f, 1.0f, 1.0f), "OUT >");
    ImNodes::EndOutputAttribute();
    ImNodes::EndNode();
    for (int i = 0; i < 7; ++i) ImNodes::PopColorStyle();

    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        const auto& state = animator.states[static_cast<size_t>(i)];
        const bool isDefault = state.name == animator.defaultStateName ||
            (animator.defaultStateName.empty() && i == 0);
        const bool isCurrent = state.name == animator.currentStateName;

        int pushedColors = 0;
        const unsigned int backgroundColor =
            NodeBackgroundColor(state.mode, isCurrent, isDefault);
        const unsigned int titleColor =
            NodeTitleColor(state.mode, isCurrent, isDefault);

        ImNodes::PushColorStyle(ImNodesCol_NodeBackground, backgroundColor);
        ++pushedColors;
        ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundHovered, IM_COL32(48, 59, 70, 255));
        ++pushedColors;
        ImNodes::PushColorStyle(ImNodesCol_NodeBackgroundSelected, IM_COL32(48, 67, 82, 255));
        ++pushedColors;
        ImNodes::PushColorStyle(ImNodesCol_NodeOutline, NodeOutlineColor(isCurrent, isDefault));
        ++pushedColors;
        ImNodes::PushColorStyle(ImNodesCol_TitleBar, titleColor);
        ++pushedColors;
        ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, IM_COL32(72, 86, 103, 255));
        ++pushedColors;
        ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, titleColor);
        ++pushedColors;

        ImNodes::BeginNode(NodeId(i));
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(state.name.empty() ? "(Unnamed)" : state.name.c_str());
        if (isCurrent) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.62f, 1.0f), "[Current]");
        } else if (isDefault) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.76f, 0.28f, 1.0f), "[Default]");
        }
        ImNodes::EndNodeTitleBar();

        ImNodes::PushColorStyle(ImNodesCol_Pin, IM_COL32(82, 164, 255, 255));
        ImNodes::PushColorStyle(ImNodesCol_PinHovered, IM_COL32(132, 210, 255, 255));
        ImNodes::BeginInputAttribute(InputPinId(i));
        inputPinScreenY[NodeId(i)] = ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.5f;
        ImGui::TextColored(ImVec4(0.42f, 0.72f, 1.0f, 1.0f), "< IN");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drop a transition here");
        ImNodes::EndInputAttribute();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();

        ImNodes::BeginStaticAttribute(NodeId(i) * 1000 + 1);
        // ノードはグラフ把握に必要な要約だけを表示し、詳細編集は Inspector に集約する。
        // WHY: フォームを各ノードへ並べるとサイズが揃わず、遷移線と状態構造が読みづらくなるため。
        const float cardWidth = BASE_NODE_CARD_WIDTH * m_canvasZoom;
        ImGui::Dummy(ImVec2(cardWidth, 0.0f));
        ImGui::TextColored(StateModeTextColor(state.mode), "%s", StateModeName(state.mode));
        ImGui::Separator();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cardWidth);

        if (state.mode == scene::AnimationStateMode::Clip) {
            ImGui::TextWrapped(
                "%s",
                state.clipName.empty() ? "No clip assigned" : state.clipName.c_str());
            ImGui::TextDisabled(
                "%s",
                state.sourcePath.empty() ? "No source" : state.sourcePath.c_str());
        } else if (state.mode == scene::AnimationStateMode::BlendTree1D) {
            ImGui::Text("Param: %s",
                state.blendTree1D.paramName.empty() ? "<none>" : state.blendTree1D.paramName.c_str());
            const auto& bt1d = state.blendTree1D;
            if (bt1d.motions.size() >= 2) {
                float minT = bt1d.motions[0].threshold, maxT = bt1d.motions[0].threshold;
                for (const auto& m : bt1d.motions) {
                    minT = std::min(minT, m.threshold);
                    maxT = std::max(maxT, m.threshold);
                }
                const float range = maxT - minT;
                const ImVec2 barMin = ImGui::GetCursorScreenPos();
                const float barH = 7.0f * m_canvasZoom;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(barMin, ImVec2(barMin.x + cardWidth, barMin.y + barH),
                    IM_COL32(30, 38, 52, 220), 2.0f * m_canvasZoom);
                if (range > 0.0f) {
                    for (const auto& m : bt1d.motions) {
                        const float tx = (m.threshold - minT) / range;
                        const float px = barMin.x + tx * cardWidth;
                        dl->AddLine(ImVec2(px, barMin.y + 1), ImVec2(px, barMin.y + barH - 1),
                            IM_COL32(110, 160, 255, 200));
                    }
                    if (isCurrent && !bt1d.paramName.empty()) {
                        const float raw = animator.GetFloat(bt1d.paramName.c_str());
                        const float tx = std::clamp((raw - minT) / range, 0.0f, 1.0f);
                        const float px = barMin.x + tx * cardWidth;
                        const float tip = 4.0f * m_canvasZoom;
                        dl->AddTriangleFilled(
                            ImVec2(px - tip, barMin.y - 2.0f * m_canvasZoom),
                            ImVec2(px + tip, barMin.y - 2.0f * m_canvasZoom),
                            ImVec2(px, barMin.y + barH + m_canvasZoom),
                            IM_COL32(255, 215, 75, 240));
                    }
                }
                ImGui::Dummy(ImVec2(cardWidth, barH));
            } else {
                ImGui::TextDisabled("%d motions", static_cast<int>(bt1d.motions.size()));
            }
            ImGui::TextDisabled("Double-click to open");
        } else {
            ImGui::Text("Parameters: %s / %s",
                state.blendTree2D.paramX.empty() ? "<none>" : state.blendTree2D.paramX.c_str(),
                state.blendTree2D.paramY.empty() ? "<none>" : state.blendTree2D.paramY.c_str());
            ImGui::TextDisabled("%d motions",
                static_cast<int>(state.blendTree2D.motions.size()));
            ImGui::TextDisabled("Double-click to open");
        }
        ImGui::PopTextWrapPos();

        ImGui::Spacing();
        ImGui::TextDisabled(
            "%s  |  Speed %.2f  |  IK %.2f",
            state.loop ? "LOOP" : "ONCE",
            state.speed,
            state.ikWeight);
        ImGui::TextDisabled(
            "%d transition%s",
            static_cast<int>(state.transitions.size()),
            state.transitions.size() == 1 ? "" : "s");
        if (isCurrent) {
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, IM_COL32(55, 210, 115, 220));
            ImGui::ProgressBar(animator.GetNormalizedTime(), ImVec2(cardWidth, 3.0f), "");
            ImGui::PopStyleColor();
        } else if (!animator.blendToState.empty() && state.name == animator.blendToState) {
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, IM_COL32(75, 140, 225, 180));
            ImGui::ProgressBar(animator.blendWeight, ImVec2(cardWidth, 3.0f), "");
            ImGui::PopStyleColor();
        }
        ImNodes::EndStaticAttribute();

        ImNodes::PushColorStyle(ImNodesCol_Pin, IM_COL32(255, 156, 72, 255));
        ImNodes::PushColorStyle(ImNodesCol_PinHovered, IM_COL32(255, 202, 118, 255));
        ImNodes::BeginOutputAttribute(OutputPinId(i));
        outputPinScreenY[NodeId(i)] = ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.5f;
        ImGui::TextColored(ImVec4(1.0f, 0.68f, 0.32f, 1.0f), "OUT >");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag from here to another state's To pin");
        ImNodes::EndOutputAttribute();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();

        ImNodes::EndNode();
        while (pushedColors-- > 0) ImNodes::PopColorStyle();
    }

    for (int from = 0; from < static_cast<int>(animator.states.size()); ++from) {
        const auto& state = animator.states[static_cast<size_t>(from)];
        for (int ti = 0; ti < static_cast<int>(state.transitions.size()); ++ti) {
            const auto& transition = state.transitions[static_cast<size_t>(ti)];
            const int to = FindStateIndexByName(animator, transition.toStateName);
            if (to < 0) continue;
            const bool isActive =
                state.name == animator.currentStateName &&
                transition.toStateName == animator.blendToState;
            const unsigned int linkColor = isActive
                ? IM_COL32(73, 232, 137, 255)
                : transition.hasExitTime
                    ? IM_COL32(228, 169, 73, 225)
                    : IM_COL32(96, 164, 224, 220);
            ImNodes::PushColorStyle(ImNodesCol_Link, linkColor);
            ImNodes::PushColorStyle(
                ImNodesCol_LinkHovered,
                isActive ? IM_COL32(123, 255, 174, 255) : IM_COL32(140, 213, 255, 255));
            ImNodes::PushColorStyle(ImNodesCol_LinkSelected, IM_COL32(255, 224, 125, 255));
            ImNodes::Link(LinkId(from, ti), OutputPinId(from), InputPinId(to));
            ImNodes::PopColorStyle();
            ImNodes::PopColorStyle();
            ImNodes::PopColorStyle();
        }
    }

    bool hasActiveStateTransition = false;
    const int currentStateIndex =
        FindStateIndexByName(animator, animator.currentStateName);
    if (currentStateIndex >= 0) {
        const auto& currentState =
            animator.states[static_cast<size_t>(currentStateIndex)];
        hasActiveStateTransition = std::any_of(
            currentState.transitions.begin(),
            currentState.transitions.end(),
            [&](const scene::AnimationTransition& transition) {
                return !animator.blendToState.empty() &&
                    transition.toStateName == animator.blendToState;
            });
    }

    for (int ti = 0; ti < static_cast<int>(animator.anyStateTransitions.size()); ++ti) {
        const auto& transition = animator.anyStateTransitions[static_cast<size_t>(ti)];
        const int to = FindStateIndexByName(
            animator, transition.toStateName);
        if (to >= 0) {
            const bool isActive =
                !hasActiveStateTransition &&
                !animator.blendToState.empty() &&
                transition.toStateName == animator.blendToState;
            ImNodes::PushColorStyle(
                ImNodesCol_Link,
                isActive ? IM_COL32(73, 232, 137, 255) : IM_COL32(177, 96, 214, 225));
            ImNodes::PushColorStyle(
                ImNodesCol_LinkHovered,
                isActive ? IM_COL32(123, 255, 174, 255) : IM_COL32(222, 144, 255, 255));
            ImNodes::PushColorStyle(ImNodesCol_LinkSelected, IM_COL32(255, 224, 125, 255));
            ImNodes::Link(AnyStateLinkId(ti), AnyStateOutputPinId(), InputPinId(to));
            ImNodes::PopColorStyle();
            ImNodes::PopColorStyle();
            ImNodes::PopColorStyle();
        }
    }

    ImNodes::PushColorStyle(ImNodesCol_Link, IM_COL32(80, 210, 128, 230));
    ImNodes::PushColorStyle(ImNodesCol_LinkHovered, IM_COL32(128, 244, 166, 255));
    ImNodes::PushColorStyle(ImNodesCol_LinkSelected, IM_COL32(255, 198, 92, 255));
    int entryStateIndex = 0;
    if (!animator.defaultStateName.empty()) {
        const int defaultIndex = FindStateIndexByName(animator, animator.defaultStateName);
        if (defaultIndex >= 0) entryStateIndex = defaultIndex;
    }
    if (entryStateIndex >= 0 && entryStateIndex < static_cast<int>(animator.states.size()))
        ImNodes::Link(EntryLinkId(), EntryOutputPinId(), InputPinId(entryStateIndex));
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();

    ImNodes::MiniMap(0.16f, ImNodesMiniMapLocation_BottomRight);
    ImNodes::EndNodeEditor();
    ImNodes::PopStyleVar(7);

    // Transition の主要条件をリンク中央へバッジ表示する。
    // WHY: 線の色だけでは Exit Time と条件遷移を区別できず、Inspector を開くまで意味が読めないため。
    auto drawTransitionBadge = [&](int linkId,
                                   int fromNodeId,
                                   int toNodeId,
                                   int parallelIndex,
                                   const scene::AnimationTransition& transition,
                                   bool isAnyState,
                                   bool isActive) {
        const bool selected = ImNodes::IsLinkSelected(linkId);
        if (m_canvasZoom < 0.65f && !selected) return;

        const ImVec2 fromPos = ImNodes::GetNodeScreenSpacePos(fromNodeId);
        const ImVec2 fromSize = ImNodes::GetNodeDimensions(fromNodeId);
        const ImVec2 toPos = ImNodes::GetNodeScreenSpacePos(toNodeId);
        const ImVec2 toSize = ImNodes::GetNodeDimensions(toNodeId);
        const ImVec2 fromCenter(
            fromPos.x + fromSize.x,
            outputPinScreenY.count(fromNodeId) ? outputPinScreenY.at(fromNodeId) : fromPos.y + fromSize.y * 0.5f);
        const ImVec2 toCenter(
            toPos.x,
            inputPinScreenY.count(toNodeId) ? inputPinScreenY.at(toNodeId) : toPos.y + toSize.y * 0.5f);
        const float offsetY =
            static_cast<float>((parallelIndex % 3) - 1) * 18.0f * m_canvasZoom;
        const ImVec2 center(
            (fromCenter.x + toCenter.x) * 0.5f,
            (fromCenter.y + toCenter.y) * 0.5f + offsetY);

        const std::string text = BuildTransitionBadge(transition);
        const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
        const ImVec2 padding(7.0f * m_canvasZoom, 4.0f * m_canvasZoom);
        const ImVec2 min(
            center.x - textSize.x * 0.5f - padding.x,
            center.y - textSize.y * 0.5f - padding.y);
        const ImVec2 max(
            center.x + textSize.x * 0.5f + padding.x,
            center.y + textSize.y * 0.5f + padding.y);

        const ImU32 background = isActive
            ? IM_COL32(28, 105, 62, 245)
            : selected
                ? IM_COL32(108, 79, 28, 245)
                : isAnyState
                    ? IM_COL32(70, 39, 82, 235)
                    : transition.hasExitTime
                        ? IM_COL32(78, 59, 27, 235)
                        : IM_COL32(30, 48, 66, 235);
        const ImU32 outline = isActive
            ? IM_COL32(91, 239, 148, 255)
            : selected
                ? IM_COL32(255, 215, 105, 255)
                : isAnyState
                    ? IM_COL32(190, 111, 224, 245)
                    : transition.hasExitTime
                        ? IM_COL32(226, 169, 75, 245)
                        : IM_COL32(99, 171, 226, 245);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max, background, 5.0f);
        drawList->AddRect(min, max, outline, 5.0f, 0, selected ? 2.0f : 1.0f);
        drawList->AddText(
            ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f),
            IM_COL32(235, 240, 246, 255),
            text.c_str());
    };

    const ImVec2 canvasWindowPos = ImGui::GetWindowPos();
    const ImVec2 canvasWindowSize = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->PushClipRect(
        canvasOrigin,
        ImVec2(
            canvasWindowPos.x + canvasWindowSize.x,
            canvasWindowPos.y + canvasWindowSize.y),
        true);
    for (int from = 0; from < static_cast<int>(animator.states.size()); ++from) {
        const auto& state = animator.states[static_cast<size_t>(from)];
        for (int ti = 0; ti < static_cast<int>(state.transitions.size()); ++ti) {
            const auto& transition = state.transitions[static_cast<size_t>(ti)];
            const int to = FindStateIndexByName(animator, transition.toStateName);
            if (to < 0) continue;
            drawTransitionBadge(
                LinkId(from, ti),
                NodeId(from),
                NodeId(to),
                ti,
                transition,
                false,
                state.name == animator.currentStateName &&
                    transition.toStateName == animator.blendToState);
        }
    }
    for (int ti = 0; ti < static_cast<int>(animator.anyStateTransitions.size()); ++ti) {
        const auto& transition = animator.anyStateTransitions[static_cast<size_t>(ti)];
        const int to = FindStateIndexByName(animator, transition.toStateName);
        if (to < 0) continue;
        drawTransitionBadge(
            AnyStateLinkId(ti),
            AnyStateNodeId(),
            NodeId(to),
            ti,
            transition,
            true,
            !hasActiveStateTransition &&
                !animator.blendToState.empty() &&
                transition.toStateName == animator.blendToState);
    }
    ImGui::GetWindowDrawList()->PopClipRect();

    if (canvasHovered && ImGui::IsKeyPressed(ImGuiKey_F) &&
        m_selectedNode >= 0 &&
        m_selectedNode < static_cast<int>(animator.states.size())) {
        ImNodes::EditorContextMoveToNode(NodeId(m_selectedNode));
    }

    const ImVec2 helpPos(
        canvasOrigin.x + 10.0f,
        canvasOrigin.y + canvasHeight - ImGui::GetTextLineHeightWithSpacing() - 10.0f);
    ImGui::GetWindowDrawList()->AddText(
        helpPos,
        IM_COL32(155, 165, 178, canvasHovered ? 230 : 145),
        "Wheel: Zoom | Shift/Alt+Wheel: Pan | Middle Drag: Pan | F: Focus");

    int activePin = 0;
    if (ImNodes::IsLinkStarted(&activePin)) {
        const bool fromOutput = (activePin % 2) == 0;
        ImGui::SetTooltip(fromOutput ? "Drop on a blue < To pin" : "Drop on an orange From > pin");
    }

    if (CanEditAnimationGraph(ctx)) {
        for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
            const auto& state = animator.states[static_cast<size_t>(i)];
            ImVec2 pos = ImNodes::GetNodeGridSpacePos(NodeId(i));
            pos.x /= m_canvasZoom;
            pos.y /= m_canvasZoom;
            ImVec2& stored = layout.nodePositions[state.name];
            if (std::fabs(stored.x - pos.x) > 0.01f || std::fabs(stored.y - pos.y) > 0.01f) {
                stored = pos;
                MarkDirty(ctx);
            }
        }
        auto storeSpecialNodePosition = [&](int nodeId, ImVec2& stored) {
            ImVec2 pos = ImNodes::GetNodeGridSpacePos(nodeId);
            pos.x /= m_canvasZoom;
            pos.y /= m_canvasZoom;
            if (std::fabs(stored.x - pos.x) > 0.01f ||
                std::fabs(stored.y - pos.y) > 0.01f) {
                stored = pos;
                MarkDirty(ctx);
            }
        };
        storeSpecialNodePosition(EntryNodeId(), layout.entryPosition);
        storeSpecialNodePosition(AnyStateNodeId(), layout.anyStatePosition);
    }

    int startedPin = 0;
    int endedPin = 0;
    if (ImNodes::IsLinkCreated(&startedPin, &endedPin)) {
        const bool fromAnyState =
            startedPin == AnyStateOutputPinId() || endedPin == AnyStateOutputPinId();
        const bool startIsOutput = (startedPin % 2) == 0;
        const bool endIsOutput = (endedPin % 2) == 0;
        const int from = startIsOutput ? (startedPin - 2) / 2 : (endedPin - 2) / 2;
        const int to = startIsOutput ? (endedPin - 1) / 2 : (startedPin - 1) / 2;
        if (fromAnyState && startIsOutput != endIsOutput &&
            to >= 0 && to < static_cast<int>(animator.states.size())) {
            const std::string& toStateName =
                animator.states[static_cast<size_t>(to)].name;
            const bool alreadyExists = std::any_of(
                animator.anyStateTransitions.begin(),
                animator.anyStateTransitions.end(),
                [&](const scene::AnimationTransition& transition) {
                    return transition.toStateName == toStateName;
                });
            if (!alreadyExists) {
                scene::AnimationTransition transition;
                transition.toStateName = toStateName;
                animator.anyStateTransitions.push_back(std::move(transition));
                m_selectedLink = {
                    -2, static_cast<int>(animator.anyStateTransitions.size()) - 1
                };
                MarkDirty(ctx);
            }
        } else if (startIsOutput != endIsOutput &&
            from >= 0 && from < static_cast<int>(animator.states.size()) &&
            to >= 0 && to < static_cast<int>(animator.states.size())) {
            AddTransition(ctx, animator, from, to);
        }
    }

    int destroyedLink = 0;
    if (ImNodes::IsLinkDestroyed(&destroyedLink)) {
        LinkRef ref = ResolveLink(destroyedLink, animator);
        if (ref.fromStateIndex == -2) {
            animator.anyStateTransitions.erase(
                animator.anyStateTransitions.begin() + ref.transitionIndex);
            m_selectedLink = {};
            MarkDirty(ctx);
        } else if (ref.fromStateIndex >= 0) {
            auto& transitions = animator.states[static_cast<size_t>(ref.fromStateIndex)].transitions;
            transitions.erase(transitions.begin() + ref.transitionIndex);
            m_selectedLink = {};
            MarkDirty(ctx);
        }
    }

    const int selectedLinkCount = ImNodes::NumSelectedLinks();
    if (selectedLinkCount > 0) {
        std::vector<int> links(static_cast<size_t>(selectedLinkCount));
        ImNodes::GetSelectedLinks(links.data());
        m_selectedLink = ResolveLink(links.front(), animator);
        m_selectedNode = -1;
        m_selectedAnyState = false;
        ImNodes::ClearNodeSelection();
    }

    const int selectedNodeCount = ImNodes::NumSelectedNodes();
    if (selectedLinkCount == 0 && selectedNodeCount > 0) {
        std::vector<int> nodes(static_cast<size_t>(selectedNodeCount));
        ImNodes::GetSelectedNodes(nodes.data());
        m_selectedAnyState = nodes.front() == AnyStateNodeId();
        m_selectedNode =
            nodes.front() >= NodeId(0) &&
            nodes.front() < NodeId(static_cast<int>(animator.states.size()))
                ? nodes.front() - 1
                : -1;
        m_selectedLink = {};
        ImNodes::ClearLinkSelection();
    }

    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        if (m_selectedLink.fromStateIndex == -2) {
            animator.anyStateTransitions.erase(
                animator.anyStateTransitions.begin() + m_selectedLink.transitionIndex);
            m_selectedLink = {};
            ImNodes::ClearLinkSelection();
            MarkDirty(ctx);
        } else if (m_selectedLink.fromStateIndex >= 0) {
            auto& transitions = animator.states[static_cast<size_t>(m_selectedLink.fromStateIndex)].transitions;
            transitions.erase(transitions.begin() + m_selectedLink.transitionIndex);
            m_selectedLink = {};
            ImNodes::ClearLinkSelection();
            MarkDirty(ctx);
        } else if (m_selectedNode >= 0 && m_selectedNode < static_cast<int>(animator.states.size())) {
            DeleteState(ctx, animator, m_selectedNode, instanceId);
            m_selectedNode = -1;
            ImNodes::ClearNodeSelection();
        }
    }
    if (ImGui::IsWindowFocused() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D) &&
        m_selectedNode >= 0 && m_selectedNode < static_cast<int>(animator.states.size())) {
        scene::AnimationState copied = animator.states[static_cast<size_t>(m_selectedNode)];
        copied.name = MakeUniqueStateName(animator, (copied.name + "_Copy").c_str());
        animator.states.push_back(std::move(copied));
        m_selectedNode = static_cast<int>(animator.states.size()) - 1;
        AutoLayoutStates(ctx, animator, instanceId);
        MarkDirty(ctx);
    }

    int hoveredNode = 0;
    const bool nodeHovered = ImNodes::IsNodeHovered(&hoveredNode);
    int hoveredLink = 0;
    const bool linkHovered = ImNodes::IsLinkHovered(&hoveredLink);
    if (nodeHovered &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
        hoveredNode >= NodeId(0) &&
        hoveredNode < NodeId(static_cast<int>(animator.states.size()))) {
        const int stateIndex = hoveredNode - NodeId(0);
        const auto mode = animator.states[static_cast<size_t>(stateIndex)].mode;
        if (mode == scene::AnimationStateMode::BlendTree1D ||
            mode == scene::AnimationStateMode::BlendTree2D) {
            m_openBlendTreeState = stateIndex;
            m_selectedMotion = -1;
            m_selectedNode = stateIndex;
            m_selectedLink = {};
            ImNodes::ClearNodeSelection();
            ImNodes::ClearLinkSelection();
            ImNodes::EditorContextResetPanning(ImVec2(0.0f, 0.0f));
        }
    }
    if (linkHovered) {
        const LinkRef hoveredRef = ResolveLink(hoveredLink, animator);
        const scene::AnimationTransition* transition = nullptr;
        const char* fromName = nullptr;
        if (hoveredRef.fromStateIndex == -2 &&
            hoveredRef.transitionIndex >= 0 &&
            hoveredRef.transitionIndex < static_cast<int>(animator.anyStateTransitions.size())) {
            transition =
                &animator.anyStateTransitions[static_cast<size_t>(hoveredRef.transitionIndex)];
            fromName = "Any State";
        } else if (hoveredRef.fromStateIndex >= 0 &&
                   hoveredRef.fromStateIndex < static_cast<int>(animator.states.size())) {
            const auto& state =
                animator.states[static_cast<size_t>(hoveredRef.fromStateIndex)];
            if (hoveredRef.transitionIndex >= 0 &&
                hoveredRef.transitionIndex < static_cast<int>(state.transitions.size())) {
                transition = &state.transitions[static_cast<size_t>(hoveredRef.transitionIndex)];
                fromName = state.name.c_str();
            }
        }

        if (transition && fromName) {
            ImGui::BeginTooltip();
            ImGui::Text("%s  ->  %s", fromName, transition->toStateName.c_str());
            ImGui::Separator();
            ImGui::Text(
                transition->fixedDuration
                    ? "Duration: %.3f s"
                    : "Duration: %.1f%% of source Length",
                transition->fixedDuration
                    ? transition->transitionDuration
                    : transition->transitionDuration * 100.0f);
            ImGui::Text(
                "Exit Time: %s",
                transition->hasExitTime ? "Enabled" : "Disabled");
            if (transition->hasExitTime)
                ImGui::Text("Exit Position: %.0f%%", transition->exitTime * 100.0f);
            if (transition->conditions.empty()) {
                ImGui::TextDisabled("No conditions");
            } else {
                ImGui::TextDisabled("Conditions (AND)");
                for (const auto& condition : transition->conditions) {
                    if (IsFloatCondition(condition.op)) {
                        ImGui::BulletText(
                            "%s %s %.2f",
                            condition.paramName.c_str(),
                            ConditionOpSymbol(condition.op),
                            condition.threshold);
                    } else {
                        ImGui::BulletText(
                            "%s %s",
                            condition.paramName.c_str(),
                            ConditionOpSymbol(condition.op));
                    }
                }
            }
            ImGui::EndTooltip();
        }
    }
    if (canvasHovered &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !nodeHovered &&
        !linkHovered) {
        // 空白クリックは Graph 要素の選択解除として扱い、通常の GameObject Inspector へ戻す。
        m_selectedNode = -1;
        m_selectedAnyState = false;
        m_selectedLink = {};
        ImNodes::ClearNodeSelection();
        ImNodes::ClearLinkSelection();
    }
    bool openRenameModal = false;
    if (nodeHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_selectedAnyState = hoveredNode == AnyStateNodeId();
        m_selectedNode = m_selectedAnyState ? -1 : hoveredNode - 1;
        m_selectedLink = {};
        ImNodes::ClearLinkSelection();
        ImGui::OpenPopup("##AnimationGraphNodeMenu");
    }
    if (ImGui::BeginPopup("##AnimationGraphNodeMenu")) {
        if (m_selectedNode >= 0 && m_selectedNode < static_cast<int>(animator.states.size())) {
            auto& state = animator.states[static_cast<size_t>(m_selectedNode)];
            if (ImGui::MenuItem("Set as Default")) {
                animator.defaultStateName = state.name;
                animator.currentStateName.clear();
                MarkDirty(ctx);
            }
            if (state.mode == scene::AnimationStateMode::BlendTree1D ||
                state.mode == scene::AnimationStateMode::BlendTree2D) {
                if (ImGui::MenuItem("Open Blend Tree")) {
                    m_openBlendTreeState = m_selectedNode;
                    m_selectedMotion = -1;
                    ImNodes::ClearNodeSelection();
                    ImNodes::EditorContextResetPanning(ImVec2(0.0f, 0.0f));
                }
            }
            if (ImGui::BeginMenu("Add Transition To")) {
                for (int to = 0; to < static_cast<int>(animator.states.size()); ++to) {
                    if (to == m_selectedNode) continue;
                    const auto& toState = animator.states[static_cast<size_t>(to)];
                    if (ImGui::MenuItem(toState.name.empty() ? "(Unnamed)" : toState.name.c_str())) {
                        AddTransition(ctx, animator, m_selectedNode, to);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Rename")) {
                m_renamingNode = m_selectedNode;
                snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s", state.name.c_str());
                openRenameModal = true;
            }
            if (ImGui::MenuItem("Duplicate")) {
                scene::AnimationState copied = state;
                copied.name = MakeUniqueStateName(animator, (state.name + "_Copy").c_str());
                animator.states.push_back(std::move(copied));
                AutoLayoutStates(ctx, animator, instanceId);
                MarkDirty(ctx);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete")) {
                DeleteState(ctx, animator, m_selectedNode, instanceId);
                m_selectedNode = -1;
            }
        }
        ImGui::EndPopup();
    }
    if (openRenameModal) ImGui::OpenPopup("##RenameStateModal");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##RenameStateModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Rename state:");
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool commit = ImGui::InputText("##renameInput", m_renameBuffer, sizeof(m_renameBuffer),
                                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (commit || ImGui::Button("OK", ImVec2(106.0f, 0.0f))) {
            if (m_renamingNode >= 0 && m_renamingNode < static_cast<int>(animator.states.size())) {
                const std::string oldName = animator.states[static_cast<size_t>(m_renamingNode)].name;
                RenameState(ctx, animator, m_renamingNode, oldName, std::string(m_renameBuffer), instanceId);
            }
            m_renamingNode = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(106.0f, 0.0f))) {
            m_renamingNode = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (linkHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        m_selectedLink = ResolveLink(hoveredLink, animator);
        m_selectedNode = -1;
        m_selectedAnyState = false;
        ImNodes::ClearNodeSelection();
        ImGui::OpenPopup("##AnimationGraphLinkMenu");
    }
    if (ImGui::BeginPopup("##AnimationGraphLinkMenu")) {
        if (m_selectedLink.fromStateIndex == -2 &&
            ImGui::MenuItem("Delete Transition")) {
            animator.anyStateTransitions.erase(
                animator.anyStateTransitions.begin() + m_selectedLink.transitionIndex);
            m_selectedLink = {};
            ImNodes::ClearLinkSelection();
            MarkDirty(ctx);
        } else if (m_selectedLink.fromStateIndex >= 0 &&
                   ImGui::MenuItem("Delete Transition")) {
            auto& transitions = animator.states[static_cast<size_t>(m_selectedLink.fromStateIndex)].transitions;
            transitions.erase(transitions.begin() + m_selectedLink.transitionIndex);
            m_selectedLink = {};
            ImNodes::ClearLinkSelection();
            MarkDirty(ctx);
        }
        ImGui::EndPopup();
    }

    if (!nodeHovered && !linkHovered && ImNodes::IsEditorHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("##AnimationGraphCanvasMenu");
    }
    if (ImGui::BeginPopup("##AnimationGraphCanvasMenu")) {
        if (ImGui::MenuItem("+ New State")) AddState(ctx, animator, "NewState");
        if (ImGui::MenuItem("Auto Layout")) AutoLayoutStates(ctx, animator, instanceId);
        if (ImGui::MenuItem("Reset Zoom")) m_canvasZoom = 1.0f;
        if (ImGui::MenuItem("Center View")) ImNodes::EditorContextResetPanning(ImVec2(0.0f, 0.0f));
        ImGui::EndPopup();
    }

    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndChild();
}

void AnimationGraphPanel::DrawBlendTreeCanvas(
    EditorContext& ctx,
    scene::AnimatorComponent& animator,
    const std::string& instanceId)
{
    if (m_openBlendTreeState < 0 ||
        m_openBlendTreeState >= static_cast<int>(animator.states.size())) {
        m_openBlendTreeState = -1;
        m_selectedMotion = -1;
        return;
    }

    auto& state = animator.states[static_cast<size_t>(m_openBlendTreeState)];
    if (state.mode != scene::AnimationStateMode::BlendTree1D &&
        state.mode != scene::AnimationStateMode::BlendTree2D) {
        m_openBlendTreeState = -1;
        m_selectedMotion = -1;
        return;
    }

    auto& motions = state.mode == scene::AnimationStateMode::BlendTree1D
        ? state.blendTree1D.motions
        : state.blendTree2D.motions;
    auto& positions =
        ctx.graphLayouts[instanceId].blendTreeMotionPositions[state.name];
    if (positions.size() < motions.size()) {
        const size_t oldSize = positions.size();
        positions.resize(motions.size());
        for (size_t i = oldSize; i < positions.size(); ++i)
            positions[i] = ImVec2(
                160.0f + static_cast<float>(i % 3) * 280.0f,
                50.0f + static_cast<float>(i / 3) * 230.0f);
    } else if (positions.size() > motions.size()) {
        positions.resize(motions.size());
    }

    ImGui::BeginChild(
        "##BlendTreeCanvas", ImVec2(0.0f, 0.0f), true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::TextColored(
        ImVec4(0.52f, 0.78f, 1.0f, 1.0f),
        "%s", state.mode == scene::AnimationStateMode::BlendTree1D
            ? "Blend Tree 1D"
            : "Blend Tree 2D");
    ImGui::SameLine();
    if (state.mode == scene::AnimationStateMode::BlendTree1D) {
        ImGui::SetNextItemWidth(180.0f);
        DrawFloatParameterCombo(
            ctx, "Parameter", animator, state.blendTree1D.paramName);
        ImGui::SameLine();
    } else {
        ImGui::SetNextItemWidth(150.0f);
        DrawFloatParameterCombo(
            ctx, "Parameter X", animator, state.blendTree2D.paramX);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        DrawFloatParameterCombo(
            ctx, "Parameter Y", animator, state.blendTree2D.paramY);
        ImGui::SameLine();
        static constexpr const char* BLEND_TYPES[] = {
            "Simple Directional", "Freeform Cartesian"
        };
        int blendType = static_cast<int>(state.blendTree2D.type);
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::Combo("Type", &blendType, BLEND_TYPES, 2)) {
            state.blendTree2D.type =
                static_cast<scene::BlendTree2DType>(blendType);
            MarkDirty(ctx);
        }
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("+ Motion")) {
        scene::BlendTreeMotion motion;
        if (!animator.clips.empty()) {
            motion.clipName = animator.clips.front().name;
            motion.clipIndex = 0;
        }
        if (state.mode == scene::AnimationStateMode::BlendTree1D &&
            !motions.empty())
            motion.threshold = motions.back().threshold + 1.0f;
        if (state.mode == scene::AnimationStateMode::BlendTree2D)
            motion.posX = static_cast<float>(motions.size());
        motions.push_back(std::move(motion));
        positions.emplace_back(
            160.0f + static_cast<float>((motions.size() - 1) % 3) * 280.0f,
            50.0f + static_cast<float>((motions.size() - 1) / 3) * 230.0f);
        m_selectedMotion = static_cast<int>(motions.size()) - 1;
        MarkDirty(ctx);
    }
    if (state.mode == scene::AnimationStateMode::BlendTree1D) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::DragFloat(
                "Damp Time", &state.blendTree1D.dampTime,
                0.01f, 0.0f, 2.0f, "%.2f s"))
            MarkDirty(ctx);
    }
    ImGui::Separator();

    if (m_nodesContext) ImNodes::SetCurrentContext(m_nodesContext);
    if (m_editorContext) ImNodes::EditorContextSet(m_editorContext);

    constexpr int ROOT_NODE_ID = 2000000;
    constexpr int ROOT_OUTPUT_ID = 2000002;
    constexpr int MOTION_NODE_BASE = 2010000;
    constexpr int MOTION_INPUT_BASE = 2020000;
    constexpr int MOTION_LINK_BASE = 2030000;
    constexpr int MOTION_STATIC_BASE = 2040000;

    ImNodes::SetNodeGridSpacePos(
        ROOT_NODE_ID, ImVec2(-220.0f * m_canvasZoom, 100.0f * m_canvasZoom));
    for (int i = 0; i < static_cast<int>(motions.size()); ++i) {
        const ImVec2 position = positions[static_cast<size_t>(i)];
        ImNodes::SetNodeGridSpacePos(
            MOTION_NODE_BASE + i,
            ImVec2(position.x * m_canvasZoom, position.y * m_canvasZoom));
    }

    ImNodes::PushStyleVar(ImNodesStyleVar_GridSpacing, 32.0f * m_canvasZoom);
    ImNodes::PushStyleVar(
        ImNodesStyleVar_NodePadding,
        ImVec2(12.0f * m_canvasZoom, 8.0f * m_canvasZoom));
    ImNodes::PushStyleVar(
        ImNodesStyleVar_NodeCornerRounding, 6.0f * m_canvasZoom);
    ImNodes::PushStyleVar(
        ImNodesStyleVar_NodeBorderThickness, 2.0f * m_canvasZoom);
    ImNodes::PushStyleVar(ImNodesStyleVar_LinkThickness, 3.0f * m_canvasZoom);
    ImNodes::PushStyleVar(
        ImNodesStyleVar_PinCircleRadius, 5.0f * m_canvasZoom);
    ImNodes::PushStyleVar(
        ImNodesStyleVar_PinHoverRadius, 9.0f * m_canvasZoom);

    ImNodes::BeginNodeEditor();
    ImGui::SetWindowFontScale(m_canvasZoom);

    ImNodes::PushColorStyle(
        ImNodesCol_NodeBackground, IM_COL32(34, 77, 98, 255));
    ImNodes::PushColorStyle(
        ImNodesCol_TitleBar, IM_COL32(39, 112, 145, 255));
    ImNodes::PushColorStyle(
        ImNodesCol_NodeOutline, IM_COL32(89, 190, 226, 255));
    ImNodes::BeginNode(ROOT_NODE_ID);
    ImNodes::BeginNodeTitleBar();
    ImGui::TextUnformatted("Blend Parameter");
    ImNodes::EndNodeTitleBar();
    ImNodes::BeginStaticAttribute(ROOT_NODE_ID + 10);
    if (state.mode == scene::AnimationStateMode::BlendTree1D) {
        ImGui::Text(
            "%s", state.blendTree1D.paramName.empty()
                ? "<Select Float Parameter>"
                : state.blendTree1D.paramName.c_str());
        ImGui::TextDisabled(
            "Raw %.3f | Blend %.3f",
            animator.GetFloat(state.blendTree1D.paramName),
            state.blendTree1D.dampedValueInitialized
                ? state.blendTree1D.dampedValue
                : animator.GetFloat(state.blendTree1D.paramName));
    } else {
        ImGui::Text(
            "X: %s", state.blendTree2D.paramX.empty()
                ? "<none>" : state.blendTree2D.paramX.c_str());
        ImGui::Text(
            "Y: %s", state.blendTree2D.paramY.empty()
                ? "<none>" : state.blendTree2D.paramY.c_str());
        ImGui::TextDisabled(
            "(%.3f, %.3f)",
            animator.GetFloat(state.blendTree2D.paramX),
            animator.GetFloat(state.blendTree2D.paramY));
    }
    ImNodes::EndStaticAttribute();
    ImNodes::BeginOutputAttribute(ROOT_OUTPUT_ID);
    ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "MOTIONS >");
    ImNodes::EndOutputAttribute();
    ImNodes::EndNode();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();

    for (int i = 0; i < static_cast<int>(motions.size()); ++i) {
        auto& motion = motions[static_cast<size_t>(i)];
        ImGui::PushID(i);
        ImNodes::PushColorStyle(
            ImNodesCol_NodeBackground, IM_COL32(52, 58, 69, 255));
        ImNodes::PushColorStyle(
            ImNodesCol_TitleBar,
            m_selectedMotion == i
                ? IM_COL32(151, 103, 38, 255)
                : IM_COL32(72, 82, 98, 255));
        ImNodes::PushColorStyle(
            ImNodesCol_NodeOutline,
            m_selectedMotion == i
                ? IM_COL32(255, 198, 92, 255)
                : IM_COL32(108, 122, 143, 255));
        ImNodes::BeginNode(MOTION_NODE_BASE + i);
        ImNodes::BeginNodeTitleBar();
        ImGui::Text(
            "Motion %d  |  %s",
            i + 1,
            motion.clipName.empty() ? "<No Clip>" : motion.clipName.c_str());
        ImNodes::EndNodeTitleBar();
        ImNodes::BeginInputAttribute(MOTION_INPUT_BASE + i);
        ImGui::TextColored(ImVec4(0.4f, 0.72f, 1.0f, 1.0f), "< WEIGHT");
        ImNodes::EndInputAttribute();
        ImNodes::BeginStaticAttribute(MOTION_STATIC_BASE + i);
        ImGui::SetNextItemWidth(220.0f * m_canvasZoom);
        DrawAnimationSource(
            ctx, "Source", animator, motion.sourcePath);
        ImGui::SetNextItemWidth(220.0f * m_canvasZoom);
        DrawClipCombo(
            ctx, "Clip", animator, motion.sourcePath,
            motion.clipName, motion.clipIndex);
        if (state.mode == scene::AnimationStateMode::BlendTree1D) {
            ImGui::SetNextItemWidth(110.0f * m_canvasZoom);
            if (ImGui::DragFloat(
                    "Threshold", &motion.threshold, 0.01f))
                MarkDirty(ctx);
        } else {
            ImGui::SetNextItemWidth(95.0f * m_canvasZoom);
            if (ImGui::DragFloat("X", &motion.posX, 0.01f))
                MarkDirty(ctx);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(95.0f * m_canvasZoom);
            if (ImGui::DragFloat("Y", &motion.posY, 0.01f))
                MarkDirty(ctx);
        }
        ImGui::SetNextItemWidth(110.0f * m_canvasZoom);
        if (ImGui::DragFloat(
                "Speed", &motion.speed, 0.01f, -10.0f, 10.0f))
            MarkDirty(ctx);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(95.0f * m_canvasZoom);
        if (ImGui::DragFloat(
                "IK", &motion.ikWeight, 0.01f, 0.0f, 1.0f))
            MarkDirty(ctx);
        const auto runtimeWeight = std::find_if(
            animator.currentBlendWeights.begin(),
            animator.currentBlendWeights.end(),
            [&](const auto& entry) {
                return entry.first == motion.clipName;
            });
        if (runtimeWeight != animator.currentBlendWeights.end()) {
            char overlay[64]{};
            std::snprintf(
                overlay, sizeof(overlay), "Weight %.1f%%",
                runtimeWeight->second * 100.0f);
            ImGui::ProgressBar(
                runtimeWeight->second,
                ImVec2(220.0f * m_canvasZoom, 4.0f * m_canvasZoom),
                overlay);
        }
        ImNodes::EndStaticAttribute();
        ImNodes::EndNode();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImNodes::PopColorStyle();
        ImGui::PopID();
        ImNodes::Link(
            MOTION_LINK_BASE + i,
            ROOT_OUTPUT_ID,
            MOTION_INPUT_BASE + i);
    }

    ImNodes::MiniMap(0.16f, ImNodesMiniMapLocation_BottomRight);
    ImNodes::EndNodeEditor();
    ImNodes::PopStyleVar(7);

    if (CanEditAnimationGraph(ctx)) {
        for (int i = 0; i < static_cast<int>(motions.size()); ++i) {
            ImVec2 position =
                ImNodes::GetNodeGridSpacePos(MOTION_NODE_BASE + i);
            position.x /= m_canvasZoom;
            position.y /= m_canvasZoom;
            ImVec2& stored = positions[static_cast<size_t>(i)];
            if (std::abs(stored.x - position.x) > 0.01f ||
                std::abs(stored.y - position.y) > 0.01f) {
                stored = position;
                MarkDirty(ctx);
            }
        }
    }

    const int selectedCount = ImNodes::NumSelectedNodes();
    if (selectedCount > 0) {
        std::vector<int> selected(static_cast<size_t>(selectedCount));
        ImNodes::GetSelectedNodes(selected.data());
        const int selectedId = selected.front();
        m_selectedMotion =
            selectedId >= MOTION_NODE_BASE &&
            selectedId < MOTION_NODE_BASE + static_cast<int>(motions.size())
                ? selectedId - MOTION_NODE_BASE
                : -1;
    }

    int duplicateMotion = -1;
    int removeMotion = -1;
    if (m_selectedMotion >= 0 &&
        m_selectedMotion < static_cast<int>(motions.size())) {
        if (ImGui::IsWindowFocused() &&
            ImGui::GetIO().KeyCtrl &&
            ImGui::IsKeyPressed(ImGuiKey_D))
            duplicateMotion = m_selectedMotion;
        if (ImGui::IsWindowFocused() &&
            ImGui::IsKeyPressed(ImGuiKey_Delete))
            removeMotion = m_selectedMotion;
    }
    if (duplicateMotion >= 0) {
        motions.insert(
            motions.begin() + duplicateMotion + 1,
            motions[static_cast<size_t>(duplicateMotion)]);
        ImVec2 copyPosition = positions[static_cast<size_t>(duplicateMotion)];
        copyPosition.x += 40.0f;
        copyPosition.y += 40.0f;
        positions.insert(
            positions.begin() + duplicateMotion + 1, copyPosition);
        m_selectedMotion = duplicateMotion + 1;
        MarkDirty(ctx);
    }
    if (removeMotion >= 0) {
        motions.erase(motions.begin() + removeMotion);
        positions.erase(positions.begin() + removeMotion);
        m_selectedMotion = -1;
        ImNodes::ClearNodeSelection();
        MarkDirty(ctx);
    }

    const ImVec2 helpPos(
        ImGui::GetWindowPos().x + 10.0f,
        ImGui::GetWindowPos().y + ImGui::GetWindowSize().y -
            ImGui::GetTextLineHeightWithSpacing() - 10.0f);
    ImGui::GetWindowDrawList()->AddText(
        helpPos,
        IM_COL32(155, 165, 178, 190),
        "Drag: Move Motion | Ctrl+D: Duplicate | Delete: Remove | Base Layer: Back");

    ImGui::SetWindowFontScale(1.0f);
    ImGui::EndChild();
}

static bool DrawAnimationGraphDetails(EditorContext& ctx,
                                       scene::AnimatorComponent& animator)
{
    auto& selection = ctx.animationGraphSelection;
    if (selection.type == EditorContext::AnimationGraphSelection::Type::None)
        return false;

    const bool isTransitionSelection =
        selection.type == EditorContext::AnimationGraphSelection::Type::Transition ||
        selection.type ==
            EditorContext::AnimationGraphSelection::Type::AnyStateTransition;
    ImGui::TextUnformatted(
        isTransitionSelection ? "Transition Inspector" : "Animation Details");
    ImGui::Separator();

    if (selection.type == EditorContext::AnimationGraphSelection::Type::State) {
        if (selection.stateIndex < 0 ||
            selection.stateIndex >= static_cast<int>(animator.states.size())) {
            selection.Clear();
            return false;
        }
        auto& state = animator.states[static_cast<size_t>(selection.stateIndex)];
        ImGui::Text("State: %s", state.name.c_str());
        DrawBlendTreeEditor(ctx, animator, state);
        return true;
    }

    if (selection.type == EditorContext::AnimationGraphSelection::Type::AnyState) {
        ImGui::TextDisabled(
            "Any State transitions are created by dragging From to a state.");
        return true;
    }

    if (selection.type == EditorContext::AnimationGraphSelection::Type::AnyStateTransition) {
        if (selection.transitionIndex < 0 ||
            selection.transitionIndex >= static_cast<int>(animator.anyStateTransitions.size())) {
            selection.Clear();
            return false;
        }
        ImGui::TextUnformatted("Any State Transition");
        DrawTransitionEditor(
            ctx,
            animator,
            nullptr,
            animator.anyStateTransitions[static_cast<size_t>(selection.transitionIndex)]);
        return true;
    }

    if (selection.type != EditorContext::AnimationGraphSelection::Type::Transition ||
        selection.stateIndex < 0 ||
        selection.stateIndex >= static_cast<int>(animator.states.size())) {
        selection.Clear();
        return false;
    }

    auto& state = animator.states[static_cast<size_t>(selection.stateIndex)];
    if (selection.transitionIndex < 0 ||
        selection.transitionIndex >= static_cast<int>(state.transitions.size())) {
        selection.Clear();
        return false;
    }

    auto& transition = state.transitions[static_cast<size_t>(selection.transitionIndex)];
    ImGui::Text("%s -> %s", state.name.c_str(), transition.toStateName.c_str());
    DrawTransitionEditor(ctx, animator, &state, transition);
    return true;
}

bool DrawAnimationGraphInspector(EditorContext& ctx, scene::GameObject& gameObject)
{
    auto& selection = ctx.animationGraphSelection;
    if (selection.entityId != gameObject.GetID()) return false;
    auto* animator = gameObject.GetComponent<scene::AnimatorComponent>();
    return animator && DrawAnimationGraphDetails(ctx, *animator);
}

bool DrawAnimationGraphAssetInspector(EditorContext& ctx)
{
    if (!ctx.animationControllerEditor ||
        ctx.animationGraphSelection.assetPath != ctx.animationControllerEditorPath)
        return false;
    return DrawAnimationGraphDetails(ctx, *ctx.animationControllerEditor);
}

static void DrawTransitionEditor(EditorContext& ctx,
                                 scene::AnimatorComponent& animator,
                                 const scene::AnimationState* sourceState,
                                 scene::AnimationTransition& transition)
{
    std::vector<const char*> stateNames;
    stateNames.reserve(animator.states.size());
    int toIndex = 0;
    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        stateNames.push_back(animator.states[static_cast<size_t>(i)].name.c_str());
        if (animator.states[static_cast<size_t>(i)].name == transition.toStateName) toIndex = i;
    }

    ImGui::SeparatorText("Transition");
    ImGui::SetNextItemWidth(180.0f);
    if (!stateNames.empty() &&
        ImGui::Combo("To State", &toIndex, stateNames.data(), static_cast<int>(stateNames.size()))) {
        transition.toStateName = stateNames[static_cast<size_t>(toIndex)];
        MarkDirty(ctx);
    }

    const scene::AnimationState* destinationState = nullptr;
    if (toIndex >= 0 && toIndex < static_cast<int>(animator.states.size()))
        destinationState = &animator.states[static_cast<size_t>(toIndex)];
    const float sourceLength = GetStateClipLength(animator, sourceState);
    const float destinationLength =
        GetStateClipLength(animator, destinationState);

    DrawTransitionTimeline(
        animator,
        sourceState,
        destinationState,
        transition,
        sourceLength,
        destinationLength);

    ImGui::SeparatorText("Settings");
    if (ImGui::Checkbox("Has Exit Time", &transition.hasExitTime)) MarkDirty(ctx);
    ImGui::BeginDisabled(!transition.hasExitTime);
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::DragFloat(
            "Exit Time", &transition.exitTime, 0.01f, 0.0f, 1.0f, "%.3f"))
        MarkDirty(ctx);
    ImGui::EndDisabled();

    if (sourceLength > 0.0f)
        ImGui::TextDisabled("Source Clip Length: %.3f s", sourceLength);
    else
        ImGui::TextDisabled("Source Clip Length: Blend Tree / unavailable");
    if (destinationLength > 0.0f)
        ImGui::TextDisabled(
            "Destination Clip Length: %.3f s", destinationLength);
    else
        ImGui::TextDisabled(
            "Destination Clip Length: Blend Tree / unavailable");

    if (ImGui::Checkbox("Fixed Duration", &transition.fixedDuration))
        MarkDirty(ctx);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Enabled: seconds\nDisabled: normalized fraction of the source Clip Length");
    if (!sourceState && !transition.fixedDuration) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.68f, 0.25f, 1.0f),
            "Any State has no source Length. Use Fixed Duration.");
    }

    ImGui::SetNextItemWidth(140.0f);
    if (transition.fixedDuration) {
        if (ImGui::DragFloat(
                "Transition Duration (s)",
                &transition.transitionDuration, 0.01f, 0.0f, 5.0f))
            MarkDirty(ctx);
    } else {
        if (ImGui::DragFloat(
                "Transition Duration",
                &transition.transitionDuration, 0.01f, 0.0f, 2.0f))
            MarkDirty(ctx);
        const float effectiveSeconds =
            sourceLength > 0.0f
                ? sourceLength * transition.transitionDuration
                : 0.0f;
        if (sourceLength > 0.0f)
            ImGui::TextDisabled(
                "Normalized: %.1f%% | Effective Blend: %.3f s",
                transition.transitionDuration * 100.0f,
                effectiveSeconds);
        else
            ImGui::TextDisabled(
                "Normalized: %.1f%% | Effective Blend unavailable",
                transition.transitionDuration * 100.0f);
    }

    ImGui::SeparatorText("Conditions");
    static constexpr const char* OP_NAMES[] = { "Greater", "Less", "Equal", "NotEqual", "True", "False" };

    int removeIndex = -1;
    for (int i = 0; i < static_cast<int>(transition.conditions.size()); ++i) {
        auto& condition = transition.conditions[static_cast<size_t>(i)];
        ImGui::PushID(i);

        std::vector<const char*> paramNames;
        paramNames.reserve(animator.parameters.size());
        int paramIndex = 0;
        for (int pi = 0; pi < static_cast<int>(animator.parameters.size()); ++pi) {
            paramNames.push_back(animator.parameters[static_cast<size_t>(pi)].name.c_str());
            if (animator.parameters[static_cast<size_t>(pi)].name == condition.paramName) paramIndex = pi;
        }

        ImGui::SetNextItemWidth(150.0f);
        if (!paramNames.empty() &&
            ImGui::Combo("##Param", &paramIndex, paramNames.data(), static_cast<int>(paramNames.size()))) {
            condition.paramName = paramNames[static_cast<size_t>(paramIndex)];
            MarkDirty(ctx);
        } else if (paramNames.empty()) {
            ImGui::TextDisabled("No parameter");
        }
        ImGui::SameLine();

        int opIndex = static_cast<int>(condition.op);
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::Combo("##Op", &opIndex, OP_NAMES, 6)) {
            condition.op = static_cast<scene::ConditionOp>(opIndex);
            MarkDirty(ctx);
        }
        ImGui::SameLine();

        if (IsFloatCondition(condition.op)) {
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::DragFloat("##Threshold", &condition.threshold, 0.01f)) MarkDirty(ctx);
            ImGui::SameLine();
        } else {
            ImGui::TextDisabled("%s", ConditionOpName(condition.op));
            ImGui::SameLine();
        }

        if (ImGui::SmallButton("x")) removeIndex = i;
        ImGui::PopID();
    }

    if (removeIndex >= 0) {
        transition.conditions.erase(transition.conditions.begin() + removeIndex);
        MarkDirty(ctx);
    }

    if (ImGui::Button("+ Condition")) {
        scene::AnimatorCondition condition;
        if (!animator.parameters.empty()) condition.paramName = animator.parameters.front().name;
        transition.conditions.push_back(std::move(condition));
        MarkDirty(ctx);
    }
}

void AnimationGraphPanel::PublishSelection(EditorContext& ctx,
                                           const scene::GameObject& gameObject) const
{
    auto& selection = ctx.animationGraphSelection;
    selection.Clear();
    selection.entityId = gameObject.GetID();

    if (m_selectedLink.fromStateIndex == -2) {
        selection.type = EditorContext::AnimationGraphSelection::Type::AnyStateTransition;
        selection.transitionIndex = m_selectedLink.transitionIndex;
    } else if (m_selectedLink.fromStateIndex >= 0) {
        selection.type = EditorContext::AnimationGraphSelection::Type::Transition;
        selection.stateIndex = m_selectedLink.fromStateIndex;
        selection.transitionIndex = m_selectedLink.transitionIndex;
    } else if (m_selectedNode >= 0) {
        selection.type = EditorContext::AnimationGraphSelection::Type::State;
        selection.stateIndex = m_selectedNode;
    } else if (m_selectedAnyState) {
        selection.type = EditorContext::AnimationGraphSelection::Type::AnyState;
    }
}

void AnimationGraphPanel::AddState(EditorContext& ctx, scene::AnimatorComponent& animator, const char* baseName)
{
    scene::AnimationState state;
    state.name = MakeUniqueStateName(animator, baseName);
    if (!animator.clips.empty()) state.clipName = animator.clips.front().name;
    if (animator.defaultStateName.empty()) animator.defaultStateName = state.name;
    animator.states.push_back(std::move(state));
    MarkDirty(ctx);
}

void AnimationGraphPanel::AddTransition(EditorContext& ctx,
                                        scene::AnimatorComponent& animator,
                                        int fromStateIndex,
                                        int toStateIndex)
{
    if (fromStateIndex < 0 || fromStateIndex >= static_cast<int>(animator.states.size())) return;
    if (toStateIndex < 0 || toStateIndex >= static_cast<int>(animator.states.size())) return;

    auto& fromState = animator.states[static_cast<size_t>(fromStateIndex)];
    const auto& toState = animator.states[static_cast<size_t>(toStateIndex)];
    for (const auto& transition : fromState.transitions) {
        if (transition.toStateName == toState.name) return;
    }

    scene::AnimationTransition transition;
    transition.toStateName = toState.name;
    fromState.transitions.push_back(std::move(transition));
    m_selectedLink = { fromStateIndex, static_cast<int>(fromState.transitions.size()) - 1 };
    ImNodes::ClearNodeSelection();
    MarkDirty(ctx);
}

void AnimationGraphPanel::AutoLayoutStates(EditorContext& ctx,
                                           scene::AnimatorComponent& animator,
                                           const std::string& instanceId)
{
    GraphLayout& layout = ctx.graphLayouts[instanceId];
    auto& positions = layout.nodePositions;
    constexpr int   COLUMNS = 4;
    constexpr float START_X = 80.0f;
    constexpr float START_Y = 80.0f;
    constexpr float STEP_X = 300.0f;
    constexpr float STEP_Y = 220.0f;

    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        const int col = i % COLUMNS;
        const int row = i / COLUMNS;
        positions[animator.states[static_cast<size_t>(i)].name] =
            ImVec2(START_X + STEP_X * static_cast<float>(col),
                   START_Y + STEP_Y * static_cast<float>(row));
    }
    layout.entryPosition = ImVec2(-220.0f, START_Y);
    layout.anyStatePosition = ImVec2(-220.0f, START_Y + STEP_Y);
    MarkDirty(ctx);
}

void AnimationGraphPanel::DeleteState(EditorContext& ctx,
                                      scene::AnimatorComponent& animator,
                                      int stateIndex,
                                      const std::string& instanceId)
{
    if (stateIndex < 0 || stateIndex >= static_cast<int>(animator.states.size())) return;
    const std::string removedName = animator.states[static_cast<size_t>(stateIndex)].name;
    animator.states.erase(animator.states.begin() + stateIndex);
    ctx.graphLayouts[instanceId].nodePositions.erase(removedName);
    ctx.graphLayouts[instanceId].blendTreeMotionPositions.erase(removedName);

    for (auto& state : animator.states) {
        state.transitions.erase(
            std::remove_if(state.transitions.begin(), state.transitions.end(),
                [&](const scene::AnimationTransition& transition) {
                    return transition.toStateName == removedName;
                }),
            state.transitions.end());
    }
    animator.anyStateTransitions.erase(
        std::remove_if(animator.anyStateTransitions.begin(),
                       animator.anyStateTransitions.end(),
            [&](const scene::AnimationTransition& transition) {
                return transition.toStateName == removedName;
            }),
        animator.anyStateTransitions.end());

    if (animator.defaultStateName == removedName) {
        animator.defaultStateName = animator.states.empty() ? std::string{} : animator.states.front().name;
    }
    if (animator.currentStateName == removedName) animator.currentStateName.clear();
    if (animator.blendToState == removedName) animator.blendToState.clear();
    m_selectedLink = {};
    MarkDirty(ctx);
}

void AnimationGraphPanel::RenameState(EditorContext& ctx,
                                      scene::AnimatorComponent& animator,
                                      int stateIndex,
                                      const std::string& oldName,
                                      const std::string& newName,
                                      const std::string& instanceId)
{
    if (stateIndex < 0 || stateIndex >= static_cast<int>(animator.states.size())) return;
    if (newName.empty() || oldName == newName) return;

    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        if (i != stateIndex && animator.states[static_cast<size_t>(i)].name == newName) return;
    }

    animator.states[static_cast<size_t>(stateIndex)].name = newName;
    for (auto& state : animator.states) {
        for (auto& transition : state.transitions) {
            if (transition.toStateName == oldName) transition.toStateName = newName;
        }
    }
    for (auto& transition : animator.anyStateTransitions)
        if (transition.toStateName == oldName) transition.toStateName = newName;

    if (animator.defaultStateName == oldName) animator.defaultStateName = newName;
    if (animator.currentStateName == oldName) animator.currentStateName = newName;
    if (animator.blendToState == oldName) animator.blendToState = newName;

    auto& positions = ctx.graphLayouts[instanceId].nodePositions;
    if (auto it = positions.find(oldName); it != positions.end()) {
        const ImVec2 renamedPosition = it->second;
        positions.erase(it);
        positions[newName] = renamedPosition;
    }
    auto& blendPositions =
        ctx.graphLayouts[instanceId].blendTreeMotionPositions;
    if (auto it = blendPositions.find(oldName); it != blendPositions.end()) {
        auto renamedPositions = std::move(it->second);
        blendPositions.erase(it);
        blendPositions[newName] = std::move(renamedPositions);
    }
    MarkDirty(ctx);
}

void AnimationGraphPanel::ClearInvalidSelection(const scene::AnimatorComponent& animator)
{
    if (m_selectedLink.fromStateIndex == -2) {
        if (m_selectedLink.transitionIndex < 0 ||
            m_selectedLink.transitionIndex >=
                static_cast<int>(animator.anyStateTransitions.size()))
            m_selectedLink = {};
        return;
    }
    if (m_selectedLink.fromStateIndex < 0) return;
    if (m_selectedLink.fromStateIndex >= static_cast<int>(animator.states.size())) {
        m_selectedLink = {};
        return;
    }
    const auto& transitions = animator.states[static_cast<size_t>(m_selectedLink.fromStateIndex)].transitions;
    if (m_selectedLink.transitionIndex < 0 ||
        m_selectedLink.transitionIndex >= static_cast<int>(transitions.size())) {
        m_selectedLink = {};
    }
}

AnimationGraphPanel::LinkRef AnimationGraphPanel::ResolveLink(int linkId,
                                                              const scene::AnimatorComponent& animator) const
{
    if ((linkId & 0x70000000) == 0x70000000) {
        const int transitionIndex = linkId & 0x0fffffff;
        if (transitionIndex >= 0 &&
            transitionIndex < static_cast<int>(animator.anyStateTransitions.size()))
            return { -2, transitionIndex };
        return {};
    }
    const int from = linkId >> 16;
    const int transitionIndex = linkId & 0xffff;
    if (from < 0 || from >= static_cast<int>(animator.states.size())) return {};

    const auto& transitions = animator.states[static_cast<size_t>(from)].transitions;
    if (transitionIndex < 0 || transitionIndex >= static_cast<int>(transitions.size())) return {};
    return { from, transitionIndex };
}

} // namespace fbzz::editor
