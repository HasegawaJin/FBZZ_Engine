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
#include <Editor/GraphEditor/GraphLayoutAlgo.hpp>
#include <Editor/GraphLayout.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui_internal.h>
// NOTE: 以前ここには `#include <imnodes.cpp>` があった。実装を .obj へ同梱することで
//       imnodes.lib のリンク漏れを回避していたが、その代償として「他のどの .cpp も
//       同じことをしてはならない」という不変条件をコメントでしか守れなくなっていた。
//       現在は fbzz_editor が imnodes を PUBLIC リンクするため不要。
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
// WHY: 大きなステートマシンを一望するには Unity 同等の広いズームレンジが必要。
constexpr float MIN_CANVAS_ZOOM = 0.30f;
constexpr float MAX_CANVAS_ZOOM = 2.00f;
constexpr float ZOOM_STEP = 0.10f;
constexpr float BASE_NODE_CARD_WIDTH = 188.0f;
// NOTE: ホイールズーム / パンの定数 (WHEEL_PAN_STEP / CANVAS_HOVER_FLAGS) は
//       旧 ImNodes 経路と一緒に削除した。ズームとパンは GraphCanvas が持つ。

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

GraphLayout ToEditorGraphLayout(const asset::AnimatorGraphLayout& source)
{
    GraphLayout layout;
    layout.entryPosition = ImVec2(source.entryPosition.x, source.entryPosition.y);
    layout.anyStatePosition = ImVec2(source.anyStatePosition.x, source.anyStatePosition.y);
    for (const auto& [stateName, pos] : source.nodePositions)
        layout.nodePositions[stateName] = ImVec2(pos.x, pos.y);
    for (const auto& [stateName, positions] : source.blendTreeMotionPositions) {
        auto& dst = layout.blendTreeMotionPositions[stateName];
        dst.reserve(positions.size());
        for (const auto& pos : positions)
            dst.emplace_back(pos.x, pos.y);
    }
    return layout;
}

asset::AnimatorGraphLayout ToAssetGraphLayout(const GraphLayout& source)
{
    asset::AnimatorGraphLayout layout;
    layout.entryPosition = { source.entryPosition.x, source.entryPosition.y };
    layout.anyStatePosition = { source.anyStatePosition.x, source.anyStatePosition.y };
    for (const auto& [stateName, pos] : source.nodePositions)
        layout.nodePositions[stateName] = { pos.x, pos.y };
    for (const auto& [stateName, positions] : source.blendTreeMotionPositions) {
        auto& dst = layout.blendTreeMotionPositions[stateName];
        dst.reserve(positions.size());
        for (const ImVec2& pos : positions)
            dst.push_back({ pos.x, pos.y });
    }
    return layout;
}

bool SaveAnimatorControllerWithLayout(EditorContext& ctx,
                                      const std::string& path,
                                      const scene::AnimatorComponent& animator)
{
    auto controller = asset::MakeAnimatorControllerAsset(animator);
    controller.editorLayout = ToAssetGraphLayout(ctx.graphLayouts[path]);
    return asset::SaveAnimatorControllerAsset(path, controller);
}

void MarkDirty(EditorContext& ctx)
{
    if (!CanEditAnimationGraph(ctx))
        return;

    ++AnimationGraphEditGeneration();
    if (util::StringUtils::EndsWith(
            ctx.selectedAssetPath, ".animcontroller")) {
        ctx.animationControllerDirty = true;
        // Registry に登録し Save All / 終了時確認で一括保存できるようにする
        const std::string capturedPath = ctx.selectedAssetPath;
        EditorContext* context = &ctx;
        std::weak_ptr<scene::AnimatorComponent> weakAnimator = ctx.animationControllerEditor;
        AssetDirtyRegistry::Register(
            capturedPath, NormalizeAssetPath(capturedPath), "CTRL",
            [capturedPath, context, weakAnimator]() {
                auto animator = weakAnimator.lock();
                if (!animator || !context) return false;
                return SaveAnimatorControllerWithLayout(*context, capturedPath, *animator);
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
    // レイヤーもグラフ定義の一部。含めないとレイヤー編集だけ Undo が効かなくなる。
    snapshot.layers = source.layers;
    snapshot.baseLayerMask.path = source.baseLayerMask.path;
    snapshot.playing = source.playing;
    return snapshot;
}

// ── レイヤーグラフの一時差し替え ─────────────────────────────────────────────
// WHY: このパネルは 130 箇所以上で animator.states / anyStateTransitions /
//      defaultStateName を直接触っている。レイヤー対応のために全箇所を
//      「今どのレイヤーか」で分岐させると、描画・選択・Undo・ノード配置の
//      すべてに条件が散り、既存の動作を壊すリスクが高い。
//
//      AnimationLayer は AnimatorComponent と同じ型のステート配列を持つので、
//      描画の前後で中身を入れ替えれば、パネル側は「常に自分のグラフを見ている」
//      ままで良い。入れ替えは 1 フレーム内で完結し、デストラクタで必ず戻す。
struct LayerGraphScope {
    scene::AnimatorComponent* animator = nullptr;
    scene::AnimationLayer*    layer    = nullptr;

    LayerGraphScope(scene::AnimatorComponent& a, const std::string& layerName)
    {
        if (layerName.empty()) return;             // Base Layer は入れ替え不要
        layer = a.FindLayer(layerName);
        if (layer == nullptr) return;              // 消えたレイヤーは Base Layer 扱い
        animator = &a;
        Swap();
    }
    ~LayerGraphScope() { Restore(); }

    LayerGraphScope(const LayerGraphScope&) = delete;
    LayerGraphScope& operator=(const LayerGraphScope&) = delete;

    [[nodiscard]] bool Active() const { return animator != nullptr; }

    // 差し替えを元へ戻す。二重呼び出ししても安全 (デストラクタと明示呼び出しの両立)。
    // WHY: Undo のスナップショット比較は「元に戻した状態」で行う必要があるため、
    //      スコープ終端を待たずに明示的に戻せる入口を用意する。
    void Restore()
    {
        if (animator == nullptr) return;
        Swap();
        animator = nullptr;
        layer = nullptr;
    }

private:
    void Swap()
    {
        std::swap(animator->states, layer->states);
        std::swap(animator->anyStateTransitions, layer->anyStateTransitions);
        std::swap(animator->defaultStateName, layer->defaultStateName);
        // ランタイム表示 (現在ステートのハイライト) もレイヤー側を見せる。
        std::swap(animator->currentStateName, layer->runtime.currentStateName);
        std::swap(animator->blendToState, layer->runtime.blendToState);
        std::swap(animator->blendWeight, layer->runtime.blendWeight);
        std::swap(animator->stateTime, layer->runtime.stateTime);
    }
};

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
    target.layers = snapshot.layers;
    target.baseLayerMask.path = snapshot.baseLayerMask.path;
    target.baseLayerMask.Invalidate();
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
    ImGui::TextDisabled("Drop an animation .asset source here.");
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
    return static_cast<float>(fallback->GetDurationSeconds());
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
        if (auto model = asset::AssetManager::LoadModel(sourcePath)) {
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
        if (ImGui::Checkbox("Sync Normalized Time", &state.blendTree1D.syncNormalizedTime))
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

// ImNodes のコンテキストは GraphCanvas が所有する。以前は旧描画経路のために
// パネル側でも 1 つ作っており、パン・ズーム・選択が二重に存在していた。
void AnimationGraphPanel::OnInit(EditorContext&) { m_graphCanvas.CreateContexts(); }

void AnimationGraphPanel::OnShutdown() { m_graphCanvas.DestroyContexts(); }

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

void AnimationGraphPanel::DrawNodeCanvas(
    EditorContext& ctx,
    scene::AnimatorComponent& animator,
    const std::string& instanceId)
{
    ImGui::BeginChild("##AnimationGenericGraph", ImVec2(0.0f, 0.0f), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    auto& layout = ctx.graphLayouts[instanceId];
    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        const auto& state = animator.states[static_cast<std::size_t>(i)];
        if (!layout.nodePositions.contains(state.name))
            layout.nodePositions[state.name] = { 80.0f + 260.0f * static_cast<float>(i), 80.0f };
    }

    GraphView view;
    GraphNodeView entry;
    entry.id = EntryNodeId();
    entry.position = layout.entryPosition;
    entry.title = "Entry";
    entry.titleColor = IM_COL32(50, 145, 82, 255);
    entry.backgroundColor = IM_COL32(35, 82, 55, 255);
    entry.inputs = {};
    entry.outputs.push_back({ EntryOutputPinId(), "START", IM_COL32(92, 220, 132, 255),
                              IM_COL32(150, 255, 178, 255), GraphPinShape::TriangleFilled });
    entry.drawBody = [this, &animator]() {
        const char* target = "<none>";
        if (!animator.defaultStateName.empty()) target = animator.defaultStateName.c_str();
        else if (!animator.states.empty()) target = animator.states.front().name.c_str();
        ImGui::TextDisabled("DEFAULT FLOW");
        ImGui::Text("Target: %s", target);
    };
    view.nodes.push_back(std::move(entry));

    GraphNodeView anyState;
    anyState.id = AnyStateNodeId();
    anyState.position = layout.anyStatePosition;
    anyState.title = "Any State";
    anyState.titleColor = IM_COL32(125, 65, 145, 255);
    anyState.backgroundColor = IM_COL32(75, 45, 85, 255);
    anyState.inputs = {};
    anyState.outputs.push_back({ AnyStateOutputPinId(), "OUT", IM_COL32(210, 118, 232, 255),
                                 IM_COL32(242, 176, 255, 255), GraphPinShape::TriangleFilled });
    anyState.drawBody = [&animator]() {
        ImGui::TextDisabled("GLOBAL TRANSITIONS");
        ImGui::Text("%d transition%s", static_cast<int>(animator.anyStateTransitions.size()),
                     animator.anyStateTransitions.size() == 1 ? "" : "s");
    };
    view.nodes.push_back(std::move(anyState));

    for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
        auto* state = &animator.states[static_cast<std::size_t>(i)];
        GraphNodeView node;
        node.id = NodeId(i);
        node.position = layout.nodePositions[state->name];
        node.title = state->name;
        node.titleColor = state->name == animator.currentStateName
            ? IM_COL32(50, 145, 82, 255)
            : (state->name == animator.defaultStateName
                ? IM_COL32(154, 107, 40, 255) : IM_COL32(72, 82, 98, 255));
        node.backgroundColor = IM_COL32(52, 58, 69, 255);
        node.outlineColor = i == m_selectedNode ? IM_COL32(255, 198, 92, 255) : 0;
        node.outlineThickness = i == m_selectedNode ? 2.5f : 0.0f;
        node.inputs.push_back({ InputPinId(i), "IN", IM_COL32(82, 164, 255, 255),
                                IM_COL32(132, 210, 255, 255), GraphPinShape::CircleFilled });
        node.outputs.push_back({ OutputPinId(i), "OUT", IM_COL32(255, 156, 72, 255),
                                 IM_COL32(255, 202, 118, 255), GraphPinShape::CircleFilled });
        node.tooltip = state->name;
        node.titleFontScale = 1.05f;
        node.drawTitle = [state, &animator]() {
            ImGui::TextUnformatted(state->name.empty() ? "(Unnamed)" : state->name.c_str());
            if (state->name == animator.currentStateName) {
                ImGui::SameLine();
                ImGui::TextColored({ 0.45f, 1.0f, 0.62f, 1.0f }, "[Current]");
            } else if (state->name == animator.defaultStateName
                       || (animator.defaultStateName.empty() && state == &animator.states.front())) {
                ImGui::SameLine();
                ImGui::TextColored({ 1.0f, 0.76f, 0.28f, 1.0f }, "[Default]");
            }
        };
        node.drawBody = [this, state, &animator]() {
            ImGui::TextColored(StateModeTextColor(state->mode), "%s", StateModeName(state->mode));
            ImGui::Separator();
            switch (state->mode) {
            case scene::AnimationStateMode::Clip:
                ImGui::TextWrapped("%s", state->clipName.empty() ? "No clip assigned" : state->clipName.c_str());
                ImGui::TextDisabled("%s", state->sourcePath.empty() ? "No source" : state->sourcePath.c_str());
                break;
            case scene::AnimationStateMode::BlendTree1D:
                ImGui::Text("Param: %s", state->blendTree1D.paramName.empty() ? "<none>" : state->blendTree1D.paramName.c_str());
                ImGui::TextDisabled("%d motions | Double-click to open", static_cast<int>(state->blendTree1D.motions.size()));
                break;
            case scene::AnimationStateMode::BlendTree2D:
                ImGui::Text("Parameters: %s / %s",
                    state->blendTree2D.paramX.empty() ? "<none>" : state->blendTree2D.paramX.c_str(),
                    state->blendTree2D.paramY.empty() ? "<none>" : state->blendTree2D.paramY.c_str());
                ImGui::TextDisabled("%d motions | Double-click to open", static_cast<int>(state->blendTree2D.motions.size()));
                break;
            }
            ImGui::Spacing();
            ImGui::TextDisabled("%s | Speed %.2f | IK %.2f", state->loop ? "LOOP" : "ONCE", state->speed, state->ikWeight);
            ImGui::TextDisabled("%d transition%s", static_cast<int>(state->transitions.size()), state->transitions.size() == 1 ? "" : "s");
            if (state->name == animator.currentStateName)
                ImGui::ProgressBar(animator.GetNormalizedTime(), { -1.0f, 5.0f }, "");
        };
        view.nodes.push_back(std::move(node));
    }

    for (int from = 0; from < static_cast<int>(animator.states.size()); ++from) {
        const auto& state = animator.states[static_cast<std::size_t>(from)];
        for (int ti = 0; ti < static_cast<int>(state.transitions.size()); ++ti) {
            const auto& transition = state.transitions[static_cast<std::size_t>(ti)];
            const int to = FindStateIndexByName(animator, transition.toStateName);
            if (to < 0) continue;
            GraphLinkView link;
            link.id = LinkId(from, ti);
            link.fromPin = OutputPinId(from);
            link.toPin = InputPinId(to);
            link.color = transition.hasExitTime ? IM_COL32(228, 169, 73, 225) : IM_COL32(96, 164, 224, 220);
            link.hoveredColor = IM_COL32(140, 213, 255, 255);
            link.selectedColor = IM_COL32(255, 224, 125, 255);
            link.arrowSize = 7.0f;
            view.links.push_back(link);
        }
    }
    for (int ti = 0; ti < static_cast<int>(animator.anyStateTransitions.size()); ++ti) {
        const int to = FindStateIndexByName(animator, animator.anyStateTransitions[static_cast<std::size_t>(ti)].toStateName);
        if (to < 0) continue;
        GraphLinkView link;
        link.id = AnyStateLinkId(ti);
        link.fromPin = AnyStateOutputPinId();
        link.toPin = InputPinId(to);
        link.color = IM_COL32(177, 96, 214, 225);
        link.hoveredColor = IM_COL32(222, 144, 255, 255);
        link.selectedColor = IM_COL32(255, 224, 125, 255);
        link.pattern = GraphLinkPattern::Dashed;
        link.arrowSize = 7.0f;
        view.links.push_back(link);
    }
    int entryTarget = FindStateIndexByName(animator, animator.defaultStateName);
    if (entryTarget < 0 && !animator.states.empty()) entryTarget = 0;
    if (entryTarget >= 0) {
        GraphLinkView link;
        link.id = EntryLinkId();
        link.fromPin = EntryOutputPinId();
        link.toPin = InputPinId(entryTarget);
        link.color = IM_COL32(80, 210, 128, 230);
        link.hoveredColor = IM_COL32(128, 244, 166, 255);
        link.selectedColor = IM_COL32(255, 198, 92, 255);
        link.arrowSize = 7.0f;
        view.links.push_back(link);
    }

    m_graphCanvas.SetZoom(m_canvasZoom);
    GraphCanvas::Config config;
    config.id = "##AnimationGenericGraphCanvas";
    config.showMiniMap = true;
    config.showGrid = true;
    config.editable = CanEditAnimationGraph(ctx);
    const GraphInteraction interaction = m_graphCanvas.Draw(view, config);
    m_canvasZoom = m_graphCanvas.Zoom();

    if (interaction.selectionChanged) {
        m_selectedNode = -1;
        m_selectedAnyState = false;
        m_selectedLink = {};
        if (!interaction.selectedLinks.empty()) {
            m_selectedLink = ResolveLink(interaction.selectedLinks.front(), animator);
        } else if (!interaction.selectedNodes.empty()) {
            const int selectedId = interaction.selectedNodes.front();
            if (selectedId == AnyStateNodeId()) m_selectedAnyState = true;
            else if (selectedId != EntryNodeId()) {
                for (int i = 0; i < static_cast<int>(animator.states.size()); ++i)
                    if (NodeId(i) == selectedId) m_selectedNode = i;
            }
        }
        m_selectionOwnerInstanceId = instanceId;
    }
    if (interaction.nodeDoubleClicked) {
        for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
            const auto mode = animator.states[static_cast<std::size_t>(i)].mode;
            if (NodeId(i) == interaction.doubleClickedNode) {
                if (mode == scene::AnimationStateMode::BlendTree1D
                    || mode == scene::AnimationStateMode::BlendTree2D) {
                    m_openBlendTreeState = i;
                    m_selectedMotion = -1;
                    m_graphCanvas.ClearSelection();
                } else {
                    m_renamingNode = i;
                    std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s",
                                  animator.states[static_cast<std::size_t>(i)].name.c_str());
                    ImGui::OpenPopup("##AnimationGenericRename");
                }
                break;
            }
        }
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F2)
        && m_selectedNode >= 0 && m_selectedNode < static_cast<int>(animator.states.size())) {
        m_renamingNode = m_selectedNode;
        std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s",
                      animator.states[static_cast<std::size_t>(m_selectedNode)].name.c_str());
        ImGui::OpenPopup("##AnimationGenericRename");
    }

    bool moved = false;
    for (const auto& move : interaction.movedNodes) {
        if (move.nodeId == EntryNodeId()) layout.entryPosition = move.position;
        else if (move.nodeId == AnyStateNodeId()) layout.anyStatePosition = move.position;
        else {
            for (int i = 0; i < static_cast<int>(animator.states.size()); ++i) {
                if (NodeId(i) == move.nodeId) {
                    layout.nodePositions[animator.states[static_cast<std::size_t>(i)].name] = move.position;
                    moved = true;
                    break;
                }
            }
        }
        moved = true;
    }
    if (moved) MarkDirty(ctx);

    auto stateIndexFromInput = [&](int pin) {
        for (int i = 0; i < static_cast<int>(animator.states.size()); ++i)
            if (InputPinId(i) == pin) return i;
        return -1;
    };
    auto sourceFromOutput = [&](int pin) {
        if (pin == EntryOutputPinId()) return -3;
        if (pin == AnyStateOutputPinId()) return -2;
        for (int i = 0; i < static_cast<int>(animator.states.size()); ++i)
            if (OutputPinId(i) == pin) return i;
        return -1;
    };
    if (interaction.linkCreated) {
        const int source = sourceFromOutput(interaction.createdFromPin);
        const int target = stateIndexFromInput(interaction.createdToPin);
        if (source == -3 && target >= 0) {
            animator.defaultStateName = animator.states[static_cast<std::size_t>(target)].name;
            MarkDirty(ctx);
        } else if (source == -2 && target >= 0) {
            const auto& targetName = animator.states[static_cast<std::size_t>(target)].name;
            const bool exists = std::any_of(animator.anyStateTransitions.begin(), animator.anyStateTransitions.end(),
                [&targetName](const scene::AnimationTransition& t) { return t.toStateName == targetName; });
            if (!exists) {
                animator.anyStateTransitions.push_back({});
                animator.anyStateTransitions.back().toStateName = targetName;
                m_selectedLink = { -2, static_cast<int>(animator.anyStateTransitions.size()) - 1 };
                MarkDirty(ctx);
            }
        } else if (source >= 0 && target >= 0) {
            AddTransition(ctx, animator, source, target);
        }
    }
    for (const int linkId : interaction.destroyedLinks) {
        const LinkRef link = ResolveLink(linkId, animator);
        if (link.fromStateIndex == -2 && link.transitionIndex >= 0) {
            animator.anyStateTransitions.erase(animator.anyStateTransitions.begin() + link.transitionIndex);
            m_selectedLink = {};
            MarkDirty(ctx);
        } else if (link.fromStateIndex >= 0 && link.transitionIndex >= 0) {
            auto& transitions = animator.states[static_cast<std::size_t>(link.fromStateIndex)].transitions;
            transitions.erase(transitions.begin() + link.transitionIndex);
            m_selectedLink = {};
            MarkDirty(ctx);
        }
    }
    if (interaction.deleteRequested) {
        if (m_selectedNode >= 0) DeleteState(ctx, animator, m_selectedNode, instanceId);
        else if (m_selectedLink.fromStateIndex == -2 && m_selectedLink.transitionIndex >= 0) {
            animator.anyStateTransitions.erase(animator.anyStateTransitions.begin() + m_selectedLink.transitionIndex);
            m_selectedLink = {};
            MarkDirty(ctx);
        } else if (m_selectedLink.fromStateIndex >= 0 && m_selectedLink.transitionIndex >= 0) {
            auto& transitions = animator.states[static_cast<std::size_t>(m_selectedLink.fromStateIndex)].transitions;
            transitions.erase(transitions.begin() + m_selectedLink.transitionIndex);
            m_selectedLink = {};
            MarkDirty(ctx);
        }
    }
    if (interaction.duplicateRequested && m_selectedNode >= 0)
        DuplicateState(ctx, animator, m_selectedNode, instanceId);

    if (interaction.contextMenuRequested) {
        m_contextSpawnX = interaction.contextSpawnPosition.x;
        m_contextSpawnY = interaction.contextSpawnPosition.y;
        ImGui::OpenPopup("##AnimationGenericCanvasMenu");
    }
    if (interaction.nodeContextMenuRequested) {
        if (interaction.contextMenuNode >= 1 && interaction.contextMenuNode < 1000000)
            m_selectedNode = interaction.contextMenuNode - 1;
        else if (interaction.contextMenuNode == AnyStateNodeId()) m_selectedAnyState = true;
        ImGui::OpenPopup("##AnimationGenericNodeMenu");
    }
    if (ImGui::BeginPopup("##AnimationGenericCanvasMenu")) {
        if (ImGui::MenuItem("+ New State"))
            AddStateAt(ctx, animator, "NewState", instanceId, m_contextSpawnX, m_contextSpawnY);
        if (ImGui::MenuItem("Auto Layout")) AutoLayoutStates(ctx, animator, instanceId);
        if (ImGui::MenuItem("Center View")) m_graphCanvas.ResetView();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##AnimationGenericNodeMenu")) {
        if (m_selectedNode >= 0 && m_selectedNode < static_cast<int>(animator.states.size())) {
            auto& state = animator.states[static_cast<std::size_t>(m_selectedNode)];
            if (ImGui::MenuItem("Set as Default")) {
                animator.defaultStateName = state.name;
                MarkDirty(ctx);
            }
            if ((state.mode == scene::AnimationStateMode::BlendTree1D || state.mode == scene::AnimationStateMode::BlendTree2D)
                && ImGui::MenuItem("Open Blend Tree")) {
                m_openBlendTreeState = m_selectedNode;
                m_selectedMotion = -1;
                m_graphCanvas.ClearSelection();
            }
            if (ImGui::MenuItem("Duplicate")) DuplicateState(ctx, animator, m_selectedNode, instanceId);
            if (ImGui::MenuItem("Delete")) DeleteState(ctx, animator, m_selectedNode, instanceId);
            if (ImGui::MenuItem("Rename")) {
                m_renamingNode = m_selectedNode;
                std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%s", state.name.c_str());
                ImGui::OpenPopup("##AnimationGenericRename");
            }
        } else if (m_selectedAnyState) {
            ImGui::TextDisabled("Any State");
        }
        ImGui::EndPopup();
    }
    if (m_renamingNode >= 0 && ImGui::BeginPopup("##AnimationGenericRename")) {
        if (ImGui::InputText("Name", m_renameBuffer, sizeof(m_renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (m_renamingNode < static_cast<int>(animator.states.size()))
                RenameState(ctx, animator, m_renamingNode, animator.states[static_cast<std::size_t>(m_renamingNode)].name,
                            m_renameBuffer, instanceId);
            m_renamingNode = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();
}

void AnimationGraphPanel::DrawBlendTreeCanvas(
    EditorContext& ctx,
    scene::AnimatorComponent& animator,
    const std::string& instanceId)
{
    if (m_openBlendTreeState < 0 || m_openBlendTreeState >= static_cast<int>(animator.states.size())) {
        m_openBlendTreeState = -1;
        m_selectedMotion = -1;
        return;
    }
    auto& state = animator.states[static_cast<std::size_t>(m_openBlendTreeState)];
    if (state.mode != scene::AnimationStateMode::BlendTree1D
        && state.mode != scene::AnimationStateMode::BlendTree2D) {
        m_openBlendTreeState = -1;
        m_selectedMotion = -1;
        return;
    }
    auto& motions = state.mode == scene::AnimationStateMode::BlendTree1D
        ? state.blendTree1D.motions : state.blendTree2D.motions;
    auto& positions = ctx.graphLayouts[instanceId].blendTreeMotionPositions[state.name];
    positions.resize(motions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (positions[i].x == 0.0f && positions[i].y == 0.0f)
            positions[i] = { 160.0f + static_cast<float>(i % 3) * 280.0f,
                              50.0f + static_cast<float>(i / 3) * 230.0f };
    }

    ImGui::BeginChild("##BlendTreeGeneric", ImVec2(0.0f, 0.0f), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::TextColored({ 0.52f, 0.78f, 1.0f, 1.0f }, "%s",
        state.mode == scene::AnimationStateMode::BlendTree1D ? "Blend Tree 1D" : "Blend Tree 2D");
    ImGui::SameLine();
    if (state.mode == scene::AnimationStateMode::BlendTree1D) {
        ImGui::SetNextItemWidth(180.0f);
        DrawFloatParameterCombo(ctx, "Parameter", animator, state.blendTree1D.paramName);
    } else {
        ImGui::SetNextItemWidth(150.0f);
        DrawFloatParameterCombo(ctx, "Parameter X", animator, state.blendTree2D.paramX);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        DrawFloatParameterCombo(ctx, "Parameter Y", animator, state.blendTree2D.paramY);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Motion")) {
        scene::BlendTreeMotion motion;
        if (!animator.clips.empty()) { motion.clipName = animator.clips.front().name; motion.clipIndex = 0; }
        motions.push_back(std::move(motion));
        positions.push_back({ 160.0f, 50.0f });
        m_selectedMotion = static_cast<int>(motions.size()) - 1;
        MarkDirty(ctx);
    }
    ImGui::Separator();

    constexpr int ROOT_NODE_ID = 2000000;
    constexpr int ROOT_OUTPUT_ID = 2000002;
    constexpr int MOTION_NODE_BASE = 2010000;
    constexpr int MOTION_INPUT_BASE = 2020000;
    constexpr int MOTION_LINK_BASE = 2030000;
    GraphView view;
    GraphNodeView root;
    root.id = ROOT_NODE_ID;
    root.position = { -220.0f, 100.0f };
    root.title = "Blend Parameter";
    root.titleColor = IM_COL32(39, 112, 145, 255);
    root.backgroundColor = IM_COL32(34, 77, 98, 255);
    root.inputs = {};
    root.outputs.push_back({ ROOT_OUTPUT_ID, "MOTIONS", IM_COL32(104, 218, 255, 255),
                             IM_COL32(164, 240, 255, 255), GraphPinShape::TriangleFilled });
    root.drawBody = [&state, &animator]() {
        if (state.mode == scene::AnimationStateMode::BlendTree1D) {
            ImGui::Text("%s", state.blendTree1D.paramName.empty() ? "<Select Float Parameter>" : state.blendTree1D.paramName.c_str());
            ImGui::TextDisabled("Raw %.3f", animator.GetFloat(state.blendTree1D.paramName));
        } else {
            ImGui::Text("X: %s", state.blendTree2D.paramX.empty() ? "<none>" : state.blendTree2D.paramX.c_str());
            ImGui::Text("Y: %s", state.blendTree2D.paramY.empty() ? "<none>" : state.blendTree2D.paramY.c_str());
        }
    };
    view.nodes.push_back(std::move(root));

    for (int i = 0; i < static_cast<int>(motions.size()); ++i) {
        auto* motion = &motions[static_cast<std::size_t>(i)];
        GraphNodeView node;
        node.id = MOTION_NODE_BASE + i;
        node.position = positions[static_cast<std::size_t>(i)];
        node.title = "Motion " + std::to_string(i + 1);
        node.titleColor = m_selectedMotion == i ? IM_COL32(151, 103, 38, 255) : IM_COL32(72, 82, 98, 255);
        node.backgroundColor = IM_COL32(52, 58, 69, 255);
        node.outlineColor = m_selectedMotion == i ? IM_COL32(255, 198, 92, 255) : 0;
        node.outlineThickness = m_selectedMotion == i ? 2.5f : 0.0f;
        node.inputs.push_back({ MOTION_INPUT_BASE + i, "WEIGHT", IM_COL32(82, 164, 255, 255),
                                IM_COL32(132, 210, 255, 255), GraphPinShape::CircleFilled });
        node.drawBody = [this, &ctx, &animator, &state, motion]() {
            ImGui::SetNextItemWidth(220.0f);
            DrawAnimationSource(ctx, "Source", animator, motion->sourcePath);
            ImGui::SetNextItemWidth(220.0f);
            DrawClipCombo(ctx, "Clip", animator, motion->sourcePath, motion->clipName, motion->clipIndex);
            if (state.mode == scene::AnimationStateMode::BlendTree1D) {
                ImGui::SetNextItemWidth(110.0f);
                if (ImGui::DragFloat("Threshold", &motion->threshold, 0.01f)) MarkDirty(ctx);
            } else {
                ImGui::SetNextItemWidth(95.0f);
                if (ImGui::DragFloat("X", &motion->posX, 0.01f)) MarkDirty(ctx);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(95.0f);
                if (ImGui::DragFloat("Y", &motion->posY, 0.01f)) MarkDirty(ctx);
            }
            ImGui::TextDisabled("Speed %.2f | IK %.2f", motion->speed, motion->ikWeight);
        };
        view.nodes.push_back(std::move(node));
        view.links.push_back({ MOTION_LINK_BASE + i, ROOT_OUTPUT_ID, MOTION_INPUT_BASE + i,
                               IM_COL32(104, 170, 210, 220), IM_COL32(140, 213, 255, 255),
                               IM_COL32(255, 224, 125, 255), GraphLinkPattern::Solid,
                               -1.0f, -1.0f, -1.0f, -1.0f, 7.0f });
    }

    m_graphCanvas.SetZoom(m_canvasZoom);
    GraphCanvas::Config config;
    config.id = "##BlendTreeGenericCanvas";
    config.showMiniMap = true;
    config.showGrid = true;
    config.editable = CanEditAnimationGraph(ctx);
    const GraphInteraction interaction = m_graphCanvas.Draw(view, config);
    m_canvasZoom = m_graphCanvas.Zoom();

    if (interaction.selectionChanged) {
        m_selectedMotion = -1;
        if (!interaction.selectedNodes.empty()) {
            const int id = interaction.selectedNodes.front();
            if (id >= MOTION_NODE_BASE && id < MOTION_NODE_BASE + static_cast<int>(motions.size()))
                m_selectedMotion = id - MOTION_NODE_BASE;
        }
    }
    for (const auto& move : interaction.movedNodes) {
        if (move.nodeId >= MOTION_NODE_BASE && move.nodeId < MOTION_NODE_BASE + static_cast<int>(positions.size())) {
            positions[static_cast<std::size_t>(move.nodeId - MOTION_NODE_BASE)] = move.position;
            MarkDirty(ctx);
        }
    }

    int pendingDelete = -1;
    int pendingDuplicate = -1;
    if (interaction.deleteRequested) pendingDelete = m_selectedMotion;
    if (interaction.duplicateRequested) pendingDuplicate = m_selectedMotion;
    if (interaction.nodeContextMenuRequested) {
        const int id = interaction.contextMenuNode;
        if (id >= MOTION_NODE_BASE && id < MOTION_NODE_BASE + static_cast<int>(motions.size())) {
            m_selectedMotion = id - MOTION_NODE_BASE;
            ImGui::OpenPopup("##BlendGenericMotionMenu");
        }
    }
    if (ImGui::BeginPopup("##BlendGenericMotionMenu")) {
        if (ImGui::MenuItem("Duplicate")) pendingDuplicate = m_selectedMotion;
        if (ImGui::MenuItem("Delete")) pendingDelete = m_selectedMotion;
        ImGui::EndPopup();
    }
    if (interaction.contextMenuRequested) {
        ImGui::OpenPopup("##BlendGenericCanvasMenu");
    }
    if (ImGui::BeginPopup("##BlendGenericCanvasMenu")) {
        if (ImGui::MenuItem("+ New Motion")) {
            scene::BlendTreeMotion motion;
            motions.push_back(std::move(motion));
            positions.push_back(interaction.contextSpawnPosition);
            m_selectedMotion = static_cast<int>(motions.size()) - 1;
            MarkDirty(ctx);
        }
        if (ImGui::MenuItem("Back to Base Layer")) {
            m_openBlendTreeState = -1;
            m_selectedMotion = -1;
            m_graphCanvas.ResetView();
        }
        ImGui::EndPopup();
    }
    if (interaction.assetDropped) {
        const std::string path = NormalizeAssetPath(interaction.droppedAssetPath);
        if (util::StringUtils::EndsWith(path, ".fbx") || util::StringUtils::EndsWith(path, ".asset")
            || util::StringUtils::EndsWith(path, ".fzasset")) {
            scene::BlendTreeMotion motion;
            motion.sourcePath = path;
            motions.push_back(std::move(motion));
            positions.push_back(interaction.dropPosition);
            m_selectedMotion = static_cast<int>(motions.size()) - 1;
            MarkDirty(ctx);
        }
    }
    if (pendingDuplicate >= 0 && pendingDuplicate < static_cast<int>(motions.size())) {
        motions.insert(motions.begin() + pendingDuplicate + 1, motions[static_cast<std::size_t>(pendingDuplicate)]);
        positions.insert(positions.begin() + pendingDuplicate + 1, positions[static_cast<std::size_t>(pendingDuplicate)] + ImVec2(40.0f, 40.0f));
        m_selectedMotion = pendingDuplicate + 1;
        MarkDirty(ctx);
    }
    if (pendingDelete >= 0 && pendingDelete < static_cast<int>(motions.size())) {
        motions.erase(motions.begin() + pendingDelete);
        positions.erase(positions.begin() + pendingDelete);
        m_selectedMotion = -1;
        MarkDirty(ctx);
    }
    ImGui::EndChild();
}

void AnimationGraphPanel::OnRenderContent(EditorContext& ctx)
{
    FBZZ_PROFILE_SCOPE("AnimationGraphPanel::Render");

    // WHY: Play Mode 中は Animator の実行時状態が毎フレーム変化する。
    //      編集用 snapshot と Undo 追跡を続けると、監視表示だけで大きな CPU 負荷になる。
    const bool allowEditing = CanEditAnimationGraph(ctx);
    const bool editingControllerAsset =
        util::StringUtils::EndsWith(ctx.selectedAssetPath, ".animcontroller");
    if (editingControllerAsset) {
        if (ctx.animationControllerEditorPath != ctx.selectedAssetPath ||
            !ctx.animationControllerEditor) {
            asset::AnimatorControllerAsset controller;
            if (!asset::LoadAnimatorControllerAsset(ctx.selectedAssetPath, controller)) {
                ImGui::TextDisabled("Failed to load Animator Controller.");
                return;
            }
            ctx.animationControllerEditor =
                std::shared_ptr<scene::AnimatorComponent>(new scene::AnimatorComponent());
            asset::ApplyAnimatorControllerAsset(
                controller, *ctx.animationControllerEditor);
            ctx.animationControllerEditorPath = ctx.selectedAssetPath;
            ctx.animationControllerDirty = false;
            ctx.graphLayouts[ctx.selectedAssetPath] =
                ToEditorGraphLayout(controller.editorLayout);
            m_selectionOwnerInstanceId.clear();
            m_selectedLink = {};
            m_selectedNode = -1;
            m_openBlendTreeState = -1;
            m_selectedMotion = -1;
            m_selectedAnyState = false;
            m_pendingTransitionFrom = -1;
            m_editingLayer.clear();
            m_graphCanvas.ClearSelection();
            m_graphCanvas.ResetView();
            ctx.animationGraphSelection.Clear();
        }

        auto& animator = *ctx.animationControllerEditor;
        scene::AnimatorComponent undoBeforeAnimator;
        GraphLayout undoBeforeLayout;
        if (allowEditing) {
            // スナップショットは差し替え前 (= Base Layer が入っている状態) で取る。
            // layers も含むため、レイヤー側の編集も Undo で戻せる。
            undoBeforeAnimator = MakeAnimationGraphSnapshot(animator);
            undoBeforeLayout = ctx.graphLayouts[ctx.selectedAssetPath];
        }
        const std::uint64_t undoGenerationBefore = AnimationGraphEditGeneration();

        // 編集対象レイヤーを選ばせ、以降の描画中だけそのグラフへ差し替える。
        DrawLayerSelector(ctx, animator, allowEditing);
        LayerGraphScope layerScope(animator, m_editingLayer);

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
                m_graphCanvas.ClearSelection();
                m_graphCanvas.ResetView();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("> %s", breadcrumbName.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Save Controller")) {
            if (SaveAnimatorControllerWithLayout(ctx, ctx.selectedAssetPath, animator)) {
                ctx.animationControllerDirty = false;
                AssetDirtyRegistry::MarkClean(ctx.selectedAssetPath);
                ctx.requestAssetBrowserRefresh = true;
                if (ctx.activeScene) {
                    asset::AnimatorControllerAsset controller;
                    if (asset::LoadAnimatorControllerAsset(ctx.selectedAssetPath, controller)) {
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
        // どのレイヤーのグラフに対する選択かを Inspector へ伝える。
        selection.layerName = m_editingLayer;

        // Undo 比較の前にレイヤーグラフを元へ戻す。
        // WHY: 差し替えたままスナップショットを比較すると、Base Layer とレイヤーの
        //      ステートが入れ替わった状態が「変更」として記録されてしまう。
        layerScope.Restore();

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
        m_pendingTransitionFrom = -1;
        m_editingLayer.clear();
        ctx.animationGraphSelection.Clear();
    }

    scene::AnimatorComponent undoBeforeAnimator;
    GraphLayout undoBeforeLayout;
    if (allowEditing) {
        // スナップショットは差し替え前 (Base Layer が入っている状態) で取る。
        undoBeforeAnimator = MakeAnimationGraphSnapshot(*animator);
        undoBeforeLayout = ctx.graphLayouts[go->instanceId];
    }
    const std::uint64_t undoGenerationBefore = AnimationGraphEditGeneration();

    // シーン上の Animator でも、編集対象レイヤーを切り替えられるようにする。
    DrawLayerSelector(ctx, *animator, allowEditing);
    LayerGraphScope layerScope(*animator, m_editingLayer);

    ClearInvalidSelection(*animator);
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
            m_graphCanvas.ClearSelection();
            m_graphCanvas.ResetView();
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
    ctx.animationGraphSelection.layerName = m_editingLayer;

    // Undo 比較の前にレイヤーグラフを元へ戻す。
    layerScope.Restore();

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

// 編集対象レイヤーを選ぶツールバー。Base Layer と各 AnimationLayer を切り替える。
// WHY: レイヤーごとのステートマシンは、これが無いとノードグラフから一切触れない。
//      切り替え時は選択状態と BlendTree の掘り下げをリセットする。
//      別グラフのインデックスを持ち越すと、存在しないステートを指したまま描画してしまう。
void AnimationGraphPanel::DrawLayerSelector(
    EditorContext& ctx, scene::AnimatorComponent& animator, bool allowEditing)
{
    // 指しているレイヤーが消えていたら Base Layer へ戻す。
    if (!m_editingLayer.empty() && animator.FindLayer(m_editingLayer) == nullptr)
        m_editingLayer.clear();

    const auto resetSelection = [&]() {
        m_selectedNode = -1;
        m_selectedLink = {};
        m_selectedMotion = -1;
        m_selectedAnyState = false;
        m_openBlendTreeState = -1;
        m_pendingTransitionFrom = -1;
        m_graphCanvas.ClearSelection();
        ctx.animationGraphSelection.Clear();
    };

    ImGui::TextDisabled("Layer");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    const char* preview = m_editingLayer.empty() ? "Base Layer" : m_editingLayer.c_str();
    if (ImGui::BeginCombo("##graph_layer", preview)) {
        if (ImGui::Selectable("Base Layer", m_editingLayer.empty())) {
            if (!m_editingLayer.empty()) { m_editingLayer.clear(); resetSelection(); }
        }
        for (const auto& layer : animator.layers) {
            const bool selected = (layer.name == m_editingLayer);
            char label[192];
            std::snprintf(label, sizeof(label), "%s  [%s %.0f%%]%s",
                          layer.name.c_str(),
                          layer.mode == scene::AnimationLayerMode::Additive ? "Additive" : "Override",
                          layer.weight * 100.0f,
                          layer.states.empty() ? "  (no graph)" : "");
            if (ImGui::Selectable(label, selected) && !selected) {
                m_editingLayer = layer.name;
                resetSelection();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!allowEditing);
    if (ImGui::SmallButton("+ Layer")) {
        scene::AnimationLayer layer;
        // 名前引き API (SetLayerWeight / PlaySlot / MCP) が壊れるため同名は避ける。
        layer.name = "Layer " + std::to_string(animator.layers.size() + 1);
        for (int suffix = 1; animator.FindLayer(layer.name) != nullptr && suffix < 1000; ++suffix)
            layer.name = "Layer " + std::to_string(animator.layers.size() + 1 + suffix);
        const std::string created = layer.name;
        animator.layers.push_back(std::move(layer));
        m_editingLayer = created;
        resetSelection();
        MarkDirty(ctx);
    }
    if (!m_editingLayer.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("- Layer")) {
            const std::string target = m_editingLayer;
            animator.layers.erase(
                std::remove_if(animator.layers.begin(), animator.layers.end(),
                    [&](const scene::AnimationLayer& l) { return l.name == target; }),
                animator.layers.end());
            m_editingLayer.clear();
            resetSelection();
            MarkDirty(ctx);
        }
    }
    ImGui::EndDisabled();

    // 選択中レイヤーの要点 (weight / mode / mask) をその場で調整できるようにする。
    if (scene::AnimationLayer* layer = animator.FindLayer(m_editingLayer)) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::SliderFloat("##layer_weight", &layer->weight, 0.0f, 1.0f, "w %.2f"))
            MarkDirty(ctx);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        static constexpr const char* kModeNames[] = { "Override", "Additive" };
        int modeIndex = static_cast<int>(layer->mode);
        if (ImGui::Combo("##layer_mode", &modeIndex, kModeNames, 2)) {
            layer->mode = static_cast<scene::AnimationLayerMode>(modeIndex);
            MarkDirty(ctx);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        if (widgets::AssetPathField("##layer_mask", layer->mask.path, ".mask", ctx.projectRoot)) {
            layer->mask.Invalidate();
            MarkDirty(ctx);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("このレイヤーが効くボーンを決める .mask アセット");
    }
    ImGui::Separator();
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
        ImGui::SeparatorText("State Settings");
        if (ImGui::DragFloat("IK Weight##det", &state.ikWeight, 0.01f, 0.0f, 1.0f))
            MarkDirty(ctx);
        if (ImGui::DragFloat("Speed##det", &state.speed, 0.01f, -10.0f, 10.0f))
            MarkDirty(ctx);
        if (ImGui::Checkbox("Loop##det", &state.loop))
            MarkDirty(ctx);
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
    if (animator == nullptr) return false;
    // selection.stateIndex はグラフごとの添字。パネルと同じレイヤーへ差し替えて解決する。
    // WHY: 差し替えずに描くと、上半身レイヤーのステートを選んだのに
    //      Base Layer の同じ添字のステートを編集してしまう。
    LayerGraphScope layerScope(*animator, selection.layerName);
    return DrawAnimationGraphDetails(ctx, *animator);
}

bool DrawAnimationGraphAssetInspector(EditorContext& ctx)
{
    if (!ctx.animationControllerEditor ||
        ctx.animationGraphSelection.assetPath != ctx.animationControllerEditorPath)
        return false;
    LayerGraphScope layerScope(*ctx.animationControllerEditor,
                               ctx.animationGraphSelection.layerName);
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

void AnimationGraphPanel::AddStateAt(EditorContext& ctx,
                                     scene::AnimatorComponent& animator,
                                     const char* baseName,
                                     const std::string& instanceId,
                                     float spawnX,
                                     float spawnY)
{
    scene::AnimationState state;
    state.name = MakeUniqueStateName(animator, baseName);
    if (!animator.clips.empty()) state.clipName = animator.clips.front().name;
    if (animator.defaultStateName.empty()) animator.defaultStateName = state.name;
    // 生成前に位置を確定しておくことで、デフォルトのグリッド整列配置を経由せず即カーソル位置へ出す。
    ctx.graphLayouts[instanceId].nodePositions[state.name] = ImVec2(spawnX, spawnY);
    animator.states.push_back(std::move(state));
    m_selectedNode = static_cast<int>(animator.states.size()) - 1;
    m_selectedLink = {};
    MarkDirty(ctx);
}

void AnimationGraphPanel::DuplicateState(EditorContext& ctx,
                                         scene::AnimatorComponent& animator,
                                         int stateIndex,
                                         const std::string& instanceId)
{
    if (stateIndex < 0 || stateIndex >= static_cast<int>(animator.states.size())) return;
    const auto& source = animator.states[static_cast<size_t>(stateIndex)];
    scene::AnimationState copied = source;
    copied.name = MakeUniqueStateName(animator, (source.name + "_Copy").c_str());

    // WHY: AutoLayout で全ノードを並べ直すと手作業のレイアウトが失われる。
    //      Unity と同じく複製元の右下へずらして置くだけに留める。
    auto& positions = ctx.graphLayouts[instanceId].nodePositions;
    ImVec2 spawn(80.0f, 80.0f);
    if (const auto it = positions.find(source.name); it != positions.end())
        spawn = ImVec2(it->second.x + 44.0f, it->second.y + 44.0f);
    positions[copied.name] = spawn;

    animator.states.push_back(std::move(copied));
    m_selectedNode = static_cast<int>(animator.states.size()) - 1;
    m_selectedLink = {};
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
    m_graphCanvas.ClearSelection();
    MarkDirty(ctx);
}

void AnimationGraphPanel::AutoLayoutStates(EditorContext& ctx,
                                           scene::AnimatorComponent& animator,
                                           const std::string& instanceId)
{
    GraphLayout& layout = ctx.graphLayouts[instanceId];
    auto& positions = layout.nodePositions;
    constexpr float START_Y = 80.0f;

    // 遷移の深さを列にする共通実装を使う。
    // WHY: 以前はここだけ「4 列の単純グリッド」で、ステートの並びが遷移の構造を
    //      一切反映していなかった。同じ Auto Layout という操作なのに VFX とは
    //      結果の質が違う状態で、共通アルゴリズム自体は既にあるのに未使用だった。
    std::vector<int> nodeIds;
    std::vector<GraphLayoutEdge> edges;
    std::unordered_map<std::string, int> indexOfState;
    for (int index = 0; index < static_cast<int>(animator.states.size()); ++index) {
        // ComputeGraphLayout は 1 以上の id を要求する (0 は「未解決」の意味を持つ)。
        indexOfState[animator.states[static_cast<std::size_t>(index)].name] = index + 1;
        nodeIds.push_back(index + 1);
    }
    for (int index = 0; index < static_cast<int>(animator.states.size()); ++index) {
        const auto& state = animator.states[static_cast<std::size_t>(index)];
        for (const auto& transition : state.transitions) {
            const auto target = indexOfState.find(transition.toStateName);
            if (target != indexOfState.end()) edges.push_back({ index + 1, target->second });
        }
    }
    // 既定ステートを根にする。入次数 0 に任せると、どこからも遷移して来ない
    // 孤立ステートまで 1 列目へ並び、開始点が読めなくなる。
    std::vector<int> roots;
    if (const auto found = indexOfState.find(animator.defaultStateName);
        found != indexOfState.end())
        roots.push_back(found->second);

    GraphLayoutOptions options;
    options.rowStep = 220.0f;
    options.originY = START_Y;
    const auto computed = roots.empty() ? ComputeGraphLayout(nodeIds, edges, options)
                                        : ComputeGraphLayout(nodeIds, edges, roots, options);
    for (int index = 0; index < static_cast<int>(animator.states.size()); ++index) {
        const auto found = computed.find(index + 1);
        if (found == computed.end()) continue;
        positions[animator.states[static_cast<std::size_t>(index)].name] = found->second;
    }
    layout.entryPosition = ImVec2(-220.0f, START_Y);
    layout.anyStatePosition = ImVec2(-220.0f, START_Y + options.rowStep);
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
    if (linkId == EntryLinkId()) return { -3, 0 };
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
