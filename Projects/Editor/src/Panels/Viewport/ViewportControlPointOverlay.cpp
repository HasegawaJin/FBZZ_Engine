/// @file    ViewportControlPointOverlay.cpp
/// @brief   Scene Viewで制御点編集を開始するための案内Overlay。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include "ViewportCommon.hpp"
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Tools/ControlPointEditing.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/ControlPointAdapters.hpp>
#include <Engine/Scene/NavigationControlPoints.hpp>
#include <Engine/Scene/PresentationControlPoints.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::editor {
namespace {

struct OverlayState {
    std::uint64_t sceneGeneration = 0;
    std::string identity;
    std::string message;
    std::string statusMessage;
    bool capturedMouse = false;
};

struct ComponentView {
    const char* kind = "";
    const char* label = "";
    scene::ControlPointSourceMode source = scene::ControlPointSourceMode::INLINE;
    scene::ControlPointRuntimeMode runtime = scene::ControlPointRuntimeMode::LIVE;
    std::size_t pointCount = 0;
    bool valid = true;
    bool stale = false;
    bool hasRuntimeMode = true;
    bool statusKnown = true;
};

OverlayState& State()
{
    static OverlayState state;
    return state;
}

bool HasPointOwner(scene::GameObject& object)
{
    const auto* water = object.GetComponent<scene::WaterComponent>();
    return (water && (water->surfaceShape == scene::WaterSurfaceShape::SPLINE_LAKE
        || water->surfaceShape == scene::WaterSurfaceShape::SPLINE_RIVER))
        || object.GetComponent<scene::SplineComponent>()
        || object.GetComponent<scene::LineRendererComponent>()
        || object.GetComponent<scene::NavMeshPatrolComponent>()
        || object.GetComponent<scene::NavMeshOffMeshLinkComponent>();
}

scene::GameObject* ResolveOwner(EditorContext& ctx)
{
    if (!ctx.activeScene) return nullptr;
    if (scene::GameObject* selected = ctx.GetSelectedGO(); selected && HasPointOwner(*selected))
        return selected;
    if (ctx.controlPointOwnerGuid.empty()) return nullptr;
    scene::GameObject* owner = ctx.activeScene->FindByGuid(ctx.controlPointOwnerGuid);
    return owner && HasPointOwner(*owner) ? owner : nullptr;
}

const char* SourceLabel(scene::ControlPointSourceMode mode)
{
    switch (mode) {
    case scene::ControlPointSourceMode::INLINE: return "数値点";
    case scene::ControlPointSourceMode::POINT_OBJECTS: return "TargetPoint参照";
    default: return "不正な位置ソース";
    }
}

const char* RuntimeLabel(scene::ControlPointRuntimeMode mode)
{
    switch (mode) {
    case scene::ControlPointRuntimeMode::LIVE: return "LIVE";
    case scene::ControlPointRuntimeMode::BAKED_STATIC: return "BAKED_STATIC";
    default: return "不正な実行モード";
    }
}

void CollectComponentViews(scene::Scene& world, scene::GameObject& owner,
                           std::vector<ComponentView>& views, bool resolveStatus,
                           std::string_view resolveKind = {})
{
    const auto shouldResolveStatus = [resolveStatus, resolveKind](std::string_view kind) {
        return resolveStatus && (resolveKind.empty() || resolveKind == kind);
    };
    if (auto* water = owner.GetComponent<scene::WaterComponent>(); water
        && (water->surfaceShape == scene::WaterSurfaceShape::SPLINE_LAKE
            || water->surfaceShape == scene::WaterSurfaceShape::SPLINE_RIVER)) {
        bool valid = true;
        bool stale = false;
        const bool statusKnown = shouldResolveStatus("water");
        if (statusKnown) {
            const auto snapshot = scene::ResolveWaterControlPoints(world, owner.GetID(), *water,
                scene::ControlPointReadContext::AUTHORING);
            valid = snapshot.valid;
            stale = snapshot.bakeStale;
        }
        const char* label = water->surfaceShape == scene::WaterSurfaceShape::SPLINE_LAKE
            ? "Water Lake" : "Water River";
        views.push_back({"water", label, water->pointSourceMode, water->pointRuntimeMode,
            water->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS
                ? water->pointBindings.size() : water->splinePoints.size(),
            valid, stale, true, statusKnown});
    }
    if (auto* spline = owner.GetComponent<scene::SplineComponent>()) {
        bool valid = true;
        bool stale = false;
        const bool statusKnown = shouldResolveStatus("spline");
        if (statusKnown) {
            const auto snapshot = scene::ResolveSplinePoints(world, owner.GetID(), *spline,
                scene::ControlPointReadContext::AUTHORING);
            valid = snapshot.valid;
            stale = snapshot.bakeStale;
        }
        views.push_back({"spline", "Spline", spline->pointSourceMode, spline->pointRuntimeMode,
            spline->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS
                ? spline->pointBindings.size() : spline->points.size(),
            valid, stale, true, statusKnown});
    }
    if (auto* line = owner.GetComponent<scene::LineRendererComponent>()) {
        bool valid = true;
        bool stale = false;
        const bool statusKnown = shouldResolveStatus("line");
        if (statusKnown) {
            const auto snapshot = scene::ResolveLineControlPoints(world, owner.GetID(), *line,
                scene::ControlPointReadContext::AUTHORING);
            valid = snapshot.valid;
            stale = snapshot.bakeStale;
        }
        views.push_back({"line", "Line Renderer", line->pointSourceMode, line->pointRuntimeMode,
            line->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS
                ? line->pointBindings.size() : line->points.size(),
            valid, stale, true, statusKnown});
    }
    if (auto* patrol = owner.GetComponent<scene::NavMeshPatrolComponent>()) {
        bool valid = true;
        bool stale = false;
        const bool statusKnown = shouldResolveStatus("patrol");
        if (statusKnown) {
            const auto snapshot = scene::ResolveNavMeshPatrolPoints(world, owner.GetID(), *patrol,
                scene::ControlPointReadContext::AUTHORING);
            valid = snapshot.valid;
            stale = snapshot.bakeStale;
        }
        views.push_back({"patrol", "NavMesh Patrol", patrol->pointSourceMode, patrol->pointRuntimeMode,
            patrol->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS
                ? patrol->waypointBindings.size() : patrol->waypoints.size(),
            valid, stale, true, statusKnown});
    }
    if (auto* link = owner.GetComponent<scene::NavMeshOffMeshLinkComponent>()) {
        const bool statusKnown = shouldResolveStatus("nav_link");
        const bool valid = statusKnown
            ? scene::ResolveNavMeshOffMeshLinkEndpoints(world, owner.GetID(), *link).valid : true;
        views.push_back({"nav_link", "NavMesh Off-Mesh Link", link->pointSourceMode,
            scene::ControlPointRuntimeMode::LIVE, 2u, valid, false, false, statusKnown});
    }
}

const char* UnavailableReason(const EditorContext& ctx, bool editable)
{
    if (!ctx.activeScene) return "編集対象 Scene がありません";
    if (!editable) return "Play / Pause 中は編集できません";
    if (ctx.playMode && ctx.playMode->HasPendingRestore()) return "Play Scene の復元中です";
    if (ctx.managedBuildBusy) return "Managed build の実行中です";
    if (!ctx.undoStack || !ctx.undoStack->IsRecordingEnabled()) return "Undo 記録が利用できません";
    return nullptr;
}

OpArgs OwnerArgs(const scene::GameObject& owner, const char* kind)
{
    OpArgs args;
    args.Set("node", owner.instanceId);
    args.Set("kind", std::string(kind));
    return args;
}

bool InvokeAction(EditorContext& ctx, OverlayState& state, std::string_view opId,
                  const OpArgs& args)
{
    const OpResult result = InvokeOperator(ctx, opId, args);
    state.statusMessage.clear();
    state.message = result.ok ? std::string{} : result.message;
    return result.ok;
}

bool ConvertSource(EditorContext& ctx, OverlayState& state, scene::GameObject& owner,
                   const ComponentView& view)
{
    const std::string ownerGuid = owner.instanceId;
    OpArgs convert = OwnerArgs(owner, view.kind);
    const bool toObjects = view.source != scene::ControlPointSourceMode::POINT_OBJECTS;
    convert.Set("source", std::string(toObjects ? "POINT_OBJECTS" : "INLINE"));
    const OpResult converted = InvokeOperator(ctx, "control_points.convert", convert);
    state.statusMessage.clear();
    state.message = converted.ok ? std::string{} : converted.message;
    if (!converted.ok) return true;

    if (toObjects) {
        OpArgs edit;
        edit.Set("owner", ownerGuid);
        if (!InvokeAction(ctx, state, "control_points.edit", edit)) return true;
        const auto* targets = converted.data.Find("targets");
        if (targets && targets->IsArray() && !targets->AsArray().empty()
            && targets->AsArray().front().IsString()) {
            if (auto* target = ctx.activeScene->FindByGuid(targets->AsArray().front().AsString()))
                RevealInHierarchy(ctx, target->GetID());
        }
    } else if (ctx.controlPointOwnerGuid == ownerGuid) {
        OpArgs edit;
        edit.Set("owner", std::string{});
        static_cast<void>(InvokeAction(ctx, state, "control_points.edit", edit));
    }
    return true;
}

bool DrawOwnerView(EditorContext& ctx, OverlayState& state, scene::GameObject& owner,
                   ComponentView view, bool editable, bool blocked)
{
    const bool active = ctx.controlPointOwnerGuid == owner.instanceId;
    const bool objectSource = view.source == scene::ControlPointSourceMode::POINT_OBJECTS;
    const bool authoringAllowed = IsControlPointAuthoringAllowed(ctx);
    const bool ownerLocked = ctx.IsLocked(owner.GetID());
    ImGui::PushID(view.kind);
    ImGui::Text("%s · %zu点", view.label, view.pointCount);
    if (view.hasRuntimeMode)
        ImGui::TextDisabled("位置: %s | 実行: %s", SourceLabel(view.source), RuntimeLabel(view.runtime));
    else
        ImGui::TextDisabled("位置: %s | 実行: NavMesh Bake時に取り込み", SourceLabel(view.source));
    if (!view.statusKnown) {
        ImGui::TextDisabled("常時解析を省略しています。");
        ImGui::BeginDisabled(!editable);
        const bool diagnosePressed = ImGui::SmallButton("状態を確認");
        ImGui::EndDisabled();
        if (!editable)
            ImGui::TextDisabled("状態確認はEditモードで行えます。");
        if (diagnosePressed) {
            std::vector<ComponentView> diagnosedViews;
            CollectComponentViews(*ctx.activeScene, owner, diagnosedViews, true, view.kind);
            for (const ComponentView& diagnosed : diagnosedViews) {
                if (std::string_view(diagnosed.kind) == view.kind) {
                    view = diagnosed;
                    break;
                }
            }
            if (view.statusKnown && view.stale)
                state.statusMessage = "確認時点: 静的ベイクが古くなっています";
            else if (view.statusKnown && !view.valid)
                state.statusMessage = "確認時点: 点の参照先または座標を確認してください";
            else if (view.statusKnown)
                state.statusMessage = "確認時点: 制御点とベイクは有効です";
            else
                state.statusMessage = "状態を確認できませんでした";
            ImGui::TextWrapped("%s", state.statusMessage.c_str());
            ImGui::TextDisabled("詳細とベイク更新はInspectorで確認してください。");
            ImGui::PopID();
            return true;
        }
        if (!state.statusMessage.empty())
            ImGui::TextWrapped("%s", state.statusMessage.c_str());
        ImGui::TextDisabled("詳細とベイク更新はInspectorで確認してください。");
    } else if (view.stale)
        ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f}, "静的ベイクが古くなっています");
    else if (!view.valid)
        ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f}, "点の参照先または座標を確認してください");

    const char* editLabel = active ? "点編集を終了" : "点編集を開始";
    OpArgs edit;
    edit.Set("owner", active ? std::string{} : owner.instanceId);
    const bool editAllowed = editable && !blocked && authoringAllowed
        && (active || (objectSource && !ownerLocked));
    ImGui::BeginDisabled(!editAllowed || !CanInvokeOperator(ctx, "control_points.edit", edit));
    const bool editPressed = ImGui::SmallButton(editLabel);
    ImGui::EndDisabled();
    if (editPressed) {
        static_cast<void>(InvokeAction(ctx, state, "control_points.edit", edit));
        ImGui::PopID();
        return true;
    }
    ImGui::SameLine();

    OpArgs convert = OwnerArgs(owner, view.kind);
    const bool toObjects = !objectSource;
    convert.Set("source", std::string(toObjects ? "POINT_OBJECTS" : "INLINE"));
    const char* convertLabel = toObjects ? "TargetPointへ変換" : "数値点へ戻す";
    const bool convertAllowed = editable && !blocked && authoringAllowed && !ownerLocked;
    ImGui::BeginDisabled(!convertAllowed || !CanInvokeOperator(ctx, "control_points.convert", convert));
    const bool convertPressed = ImGui::SmallButton(convertLabel);
    ImGui::EndDisabled();
    if (convertPressed) {
        static_cast<void>(ConvertSource(ctx, state, owner, view));
        ImGui::PopID();
        return true;
    }
    ImGui::PopID();
    return false;
}

} /// @note namespace

bool DrawControlPointOverlay(EditorContext& ctx, const ImVec2& viewportMin,
                             const ImVec2& viewportSize, bool editable, bool blocked)
{
    auto& state = State();
    /// @note カード非表示中のrelease eventを取り逃がしても、再表示時にmouse占有を残さない。
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) state.capturedMouse = false;
    if (!ctx.activeScene) return false;
    scene::GameObject* owner = ResolveOwner(ctx);
    scene::GameObject* selectedPoint = nullptr;
    for (const scene::EntityID id : ctx.selectedEntities) {
        scene::GameObject* selected = ctx.activeScene->GetGameObject(id);
        if (selected && selected->GetComponent<scene::TargetPointComponent>()) {
            selectedPoint = selected;
            break;
        }
    }
    if (!owner && !selectedPoint) return false;

    const std::uint64_t generation = ctx.activeScene->GetRenderSceneGeneration();
    const std::string identity = owner ? owner->instanceId : selectedPoint->instanceId;
    if (state.sceneGeneration != generation || state.identity != identity) {
        state = {};
        state.sceneGeneration = generation;
        state.identity = identity;
    }

    std::vector<ComponentView> views;
    if (owner) {
        CollectComponentViews(*ctx.activeScene, *owner, views, false);
    }

    const float margin = 6.0f;
    const float width = (std::min)(340.0f, viewportSize.x - margin * 2.0f);
    const float toolbarRows = ctx.mapEditingMode ? 2.0f : 1.0f;
    const float top = margin + toolbarRows * ImGui::GetFrameHeight() + 12.0f;
    const float height = (std::min)(196.0f, viewportSize.y - top - margin);
    const float minimumHeight = owner ? 160.0f : 62.0f;
    if (width < 180.0f || height < minimumHeight) return false;

    const ImVec2 cardMin{viewportMin.x + margin, viewportMin.y + top};
    const ImVec2 cardMax{cardMin.x + width, cardMin.y + height};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(cardMin, cardMax, IM_COL32(19, 23, 30, 232), 5.0f);
    draw->AddRect(cardMin, cardMax, IM_COL32(115, 133, 153, 235), 5.0f, 0, 1.0f);
    ImGui::PushClipRect(cardMin, cardMax, true);
    const ImVec2 savedCursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos({cardMin.x + 8.0f, cardMin.y + 6.0f});
    const float wrapWidth = width - 16.0f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);

    ImGui::TextUnformatted("制御点");
    const char* unavailable = UnavailableReason(ctx, editable);
    if (unavailable)
        ImGui::TextColored({1.0f, 0.72f, 0.25f, 1.0f}, "編集不可: %s", unavailable);
    else if (blocked)
        ImGui::TextDisabled("別のViewportツールが入力中です");

    bool handled = false;
    if (owner) {
        if (!views.empty())
            handled = DrawOwnerView(ctx, state, *owner, views.front(), editable, blocked);
        if (!handled && views.size() > 1)
            ImGui::TextDisabled("他の点列設定はInspectorに表示されます");
        if (!state.message.empty())
            ImGui::TextColored({1.0f, 0.62f, 0.48f, 1.0f}, "%s", state.message.c_str());
        if (!handled && !views.empty()) {
            const auto& first = views.front();
            const char* help = nullptr;
            if (first.source == scene::ControlPointSourceMode::INLINE) {
                help = "変換すると、現在の位置にTargetPointを作成して順序付き参照へ接続します。";
            } else if (std::string_view(first.kind) == "line") {
                const auto* line = owner->GetComponent<scene::LineRendererComponent>();
                help = line && line->space == scene::LineSpace::Local
                    ? "Local Lineの点は所有者の子です。点をクリックして選択し、移動Gizmoで調整します。"
                    : "World Lineの点はScene rootの独立Objectです。クリックして選択し、移動Gizmoで調整します。";
            } else if (std::string_view(first.kind) == "patrol"
                || std::string_view(first.kind) == "nav_link") {
                help = "点はScene rootの独立Objectです。点をクリックして選択し、移動Gizmoで調整します。";
            } else {
                help = "点をクリックして選択し、移動Gizmoをドラッグします。回転・拡縮は対応するGizmoを選びます。";
            }
            const float remainingHeight = cardMax.y - ImGui::GetCursorScreenPos().y - 6.0f;
            const float helpHeight = ImGui::CalcTextSize(help, nullptr, false, wrapWidth).y
                + ImGui::GetStyle().ItemSpacing.y;
            if (remainingHeight >= helpHeight)
                ImGui::TextWrapped("%s", help);
        }
    } else if (!state.message.empty()) {
        ImGui::TextColored({1.0f, 0.62f, 0.48f, 1.0f}, "%s", state.message.c_str());
    }

    if (!handled && selectedPoint) {
        ImGui::Separator();
        const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
        if (cardMax.y - ImGui::GetCursorScreenPos().y - 6.0f >= lineHeight) {
            ImGui::Text("選択中: %s", selectedPoint->name.c_str());
            const char* instructions = "クリックで選択、Shiftで追加、Ctrlで切替。移動Gizmoをドラッグして位置を調整します。";
            const float instructionHeight = ImGui::CalcTextSize(instructions, nullptr, false, wrapWidth).y
                + ImGui::GetStyle().ItemSpacing.y;
            if (cardMax.y - ImGui::GetCursorScreenPos().y - 6.0f >= instructionHeight)
                ImGui::TextWrapped("%s", instructions);
        }
    }

    const bool hovered = ImGui::IsMouseHoveringRect(cardMin, cardMax);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) state.capturedMouse = true;
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) state.capturedMouse = false;
    ImGui::PopTextWrapPos();
    ImGui::SetCursorScreenPos(savedCursor);
    ImGui::PopClipRect();
    return hovered || state.capturedMouse || handled;
}

} /// @note namespace fbzz::editor
