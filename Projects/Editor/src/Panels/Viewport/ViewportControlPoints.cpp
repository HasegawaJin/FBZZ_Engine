/// @file    ViewportControlPoints.cpp
/// @brief   共通TargetPointのクリック、候補Gizmo、Water属性ハンドル。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include "ViewportCommon.hpp"
#include <Editor/Tools/ControlPointEditing.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/ClothComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp>
#include <Engine/Scene/ControlPointAdapters.hpp>
#include <Engine/Scene/NavigationControlPoints.hpp>
#include <Engine/Scene/PresentationControlPoints.hpp>
#include <Engine/Scene/SplineCurve.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fbzz::editor {
namespace {
struct PointGizmoId {
    explicit PointGizmoId(const void* id) { ImGuizmo::PushID(id); }
    ~PointGizmoId() { ImGuizmo::PopID(); }
};

bool Inside(const ImVec2& point, const ImVec2& min, const ImVec2& size)
{
    return point.x >= min.x && point.y >= min.y && point.x <= min.x + size.x && point.y <= min.y + size.y;
}

/// @note 点のクリック判定はD3Dの同次clip範囲とviewport矩形の両方を検証する。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_rasterizer_desc DepthClipEnable。
bool Project(const math::Matrix4& matrix, const ImVec2& min, const ImVec2& size,
             const math::Vector3& base, const math::Vector3& offset, ImVec2& out)
{
    if (size.x <= 0.0f || size.y <= 0.0f) return false;
    const auto clip = matrix * math::Vector4{base.x, base.y, base.z, 1.0f}
        + matrix * math::Vector4{offset.x, offset.y, offset.z, 0.0f};
    if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.z)
        || !std::isfinite(clip.w) || clip.w <= 1e-6f || clip.z < 0.0f || clip.z > clip.w
        || clip.x < -clip.w || clip.x > clip.w || clip.y < -clip.w || clip.y > clip.w) return false;
    out = {min.x + (clip.x / clip.w * 0.5f + 0.5f) * size.x,
           min.y + (0.5f - clip.y / clip.w * 0.5f) * size.y};
    return Inside(out, min, size);
}

const ControlPointPreview* PreviewFor(const ControlPointMoveSession& session, scene::EntityID entity)
{
    const auto& points = session.Preview();
    const auto found = std::lower_bound(points.begin(), points.end(), entity.index,
        [](const auto& point, uint32_t index) { return point.entity.index < index; });
    return found != points.end() && found->entity == entity ? &*found : nullptr;
}

scene::GameObject* EditOwner(EditorContext& ctx)
{
    if (!ctx.activeScene) return nullptr;
    const auto hasPoints = [](scene::GameObject& object) {
        return object.GetComponent<scene::WaterComponent>() || object.GetComponent<scene::ClothComponent>()
            || object.GetComponent<scene::SplineComponent>() || object.GetComponent<scene::LineRendererComponent>()
            || object.GetComponent<scene::NavMeshPatrolComponent>() || object.GetComponent<scene::NavMeshOffMeshLinkComponent>();
    };
    auto* selected = ctx.GetSelectedGO();
    if (selected && hasPoints(*selected)) return selected;
    if (!ctx.controlPointOwnerGuid.empty())
        if (auto* owner = ctx.activeScene->FindByGuid(ctx.controlPointOwnerGuid)) return owner;
    for (auto* object = selected ? selected->GetParent() : nullptr; object; object = object->GetParent())
        if (hasPoints(*object)) return object;
    return nullptr;
}

math::Vector3 Offset(const scene::Transform& pose, const math::Vector3& point)
{
    return pose.worldRotation * math::Vector3{point.x * pose.worldScale.x,
        point.y * pose.worldScale.y, point.z * pose.worldScale.z};
}

/// @note 形状が不正でも参照から所属を判断し、修復するための編集対象を保つ。
bool OwnerReferencesPoint(scene::GameObject& owner, const std::string& guid)
{
    const auto contains = [&guid](const auto& rows) {
        return std::any_of(rows.begin(), rows.end(), [&guid](const auto& row) { return row.target.guid == guid; });
    };
    if (const auto* water = owner.GetComponent<scene::WaterComponent>(); water
        && water->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS
        && water->surfaceShape != scene::WaterSurfaceShape::RECTANGLE && contains(water->pointBindings)) return true;
    if (const auto* spline = owner.GetComponent<scene::SplineComponent>(); spline
        && spline->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS && contains(spline->pointBindings)) return true;
    if (const auto* line = owner.GetComponent<scene::LineRendererComponent>(); line
        && line->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS && contains(line->pointBindings)) return true;
    if (const auto* patrol = owner.GetComponent<scene::NavMeshPatrolComponent>(); patrol
        && patrol->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS && contains(patrol->waypointBindings)) return true;
    if (const auto* cloth = owner.GetComponent<scene::ClothComponent>())
        for (const auto& attachment : cloth->attachments)
            if (attachment.targetReference.guid == guid) return true;
    const auto* link = owner.GetComponent<scene::NavMeshOffMeshLinkComponent>();
    return link && link->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS
        && (link->startTarget.guid == guid || link->endTarget.guid == guid);
}

bool SamePose(const scene::Transform& lhs, const scene::Transform& rhs)
{
    return lhs.position == rhs.position && lhs.rotation == rhs.rotation && lhs.scale == rhs.scale
        && lhs.worldPosition == rhs.worldPosition && lhs.worldRotation == rhs.worldRotation && lhs.worldScale == rhs.worldScale;
}

bool SameMatrix(const math::Matrix4& lhs, const math::Matrix4& rhs)
{
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            if (lhs.m[row][column] != rhs.m[row][column]) return false;
    return true;
}

struct WaterHandle {
    uint32_t id = 0;
    bool width = false;
    float value = 0.0f;
    math::Vector3 base{};
    math::Vector3 offset{};
    math::Vector3 axis{};
    ImVec2 screen{};
};

struct WorldPolyline {
    std::vector<math::Vector3> points;
    bool closed = false;
};

struct OwnerMarkerPlacement {
    scene::EntityID entity{};
    math::Vector3 base{}, offset{};
};

const OwnerMarkerPlacement* PlacementFor(const std::vector<OwnerMarkerPlacement>& placements,
    scene::EntityID entity)
{
    const auto found = std::lower_bound(placements.begin(), placements.end(), entity.index,
        [](const auto& placement, uint32_t index) { return placement.entity.index < index; });
    return found != placements.end() && found->entity == entity ? &*found : nullptr;
}

scene::EntityID BindingTarget(scene::Scene& scene, const scene::ObjectReference& reference)
{
    if (!reference.guid.empty()) {
        if (auto* object = scene.FindByGuid(reference.guid)) return object->GetID();
        return scene::EntityID::INVALID;
    }
    return scene.GetGameObject(reference.id) ? reference.id : scene::EntityID::INVALID;
}

/// @note 軸ドラッグは当該viewportの正/透視投影からレイを復元し、Rayと軸の最近接位置を使う。
bool AxisParameter(const EditorContext& ctx, const ImVec2& min, const ImVec2& size,
                   const WaterHandle& handle, float& parameter)
{
    if (!ctx.editorCamera || size.x <= 0.0f || size.y <= 0.0f) return false;
    const auto mouse = ImGui::GetMousePos();
    const float nx = (mouse.x - min.x) / size.x * 2.0f - 1.0f;
    const float ny = 1.0f - (mouse.y - min.y) / size.y * 2.0f;
    const auto relativeView = ctx.editorCamera->GetViewMatrix() * math::Matrix4::Translate(handle.base);
    const auto inverse = math::Matrix4::Inverse(ctx.editorCamera->GetProjectionMatrix() * relativeView);
    auto nearPoint = inverse * math::Vector4{nx, ny, 0.0f, 1.0f};
    auto farPoint = inverse * math::Vector4{nx, ny, 1.0f, 1.0f};
    if (!std::isfinite(nearPoint.w) || !std::isfinite(farPoint.w) || std::fabs(nearPoint.w) < 1e-6f || std::fabs(farPoint.w) < 1e-6f) return false;
    const math::Vector3 origin{nearPoint.x / nearPoint.w, nearPoint.y / nearPoint.w, nearPoint.z / nearPoint.w};
    const math::Vector3 far{farPoint.x / farPoint.w, farPoint.y / farPoint.w, farPoint.z / farPoint.w};
    const auto direction = far - origin;
    const auto relative = origin - handle.offset;
    const float aa = math::Vector3::Dot(handle.axis, handle.axis);
    const float rr = math::Vector3::Dot(direction, direction);
    const float ar = math::Vector3::Dot(handle.axis, direction);
    const float determinant = aa * rr - ar * ar;
    if (!std::isfinite(determinant) || aa < 1e-8f || rr < 1e-8f || determinant < aa * rr * 1e-5f) return false;
    parameter = (rr * math::Vector3::Dot(handle.axis, relative) - ar * math::Vector3::Dot(direction, relative)) / determinant;
    return std::isfinite(parameter);
}
} /// @note namespace

struct ControlPointViewportTool::Impl {
    ControlPointMoveSession move;
    math::Matrix4 gizmo = math::Matrix4::Identity();
    math::Vector3 origin{};
    math::Vector3 gizmoStartOffset{};
    int lastFrame = -1;
    bool wasUsing = false;
    bool untilRelease = false;
    math::Matrix4 startView, startProjection;
    ImVec2 startMin{}, startSize{};
    std::vector<math::Vector3> curve;
    std::vector<WorldPolyline> polylines;
    std::vector<OwnerMarkerPlacement> ownerMarkers;
    std::vector<WaterHandle> handles;
    std::vector<scene::EntityID> selection;
    std::unordered_set<uint64_t> selectedIds;
    struct AttributeDrag {
        uint64_t scene = 0, lifetime = 0, signature = 0, referenceSignature = 0;
        size_t revision = 0;
        std::string ownerGuid, editOwner;
        scene::EntityID owner{};
        scene::Transform pose;
        std::vector<scene::EntityID> selection;
        int gizmoMode = 0, gizmoSpace = 0, gizmoPivot = 0;
        WaterHandle handle;
        float initialParameter = 0.0f, candidate = 0.0f;
        bool active = false;
    } attribute;
};

ControlPointViewportTool::ControlPointViewportTool() : m_impl(std::make_unique<Impl>()) {}
ControlPointViewportTool::~ControlPointViewportTool() = default;
ControlPointViewportTool::ControlPointViewportTool(ControlPointViewportTool&&) noexcept = default;
ControlPointViewportTool& ControlPointViewportTool::operator=(ControlPointViewportTool&&) noexcept = default;

void ControlPointViewportTool::Cancel()
{
    if (!m_impl) return;
    m_impl->move.Cancel(); m_impl->attribute.active = false; m_impl->wasUsing = false;
}

bool ControlPointViewportTool::Update(EditorContext& ctx, const ImVec2& min, const ImVec2& size,
                                     bool editable, bool blocked, bool hovered)
{
    auto& state = *m_impl;
    const int frame = ImGui::GetFrameCount();
    PointGizmoId gizmoId(m_impl.get());
    const bool inside = Inside(ImGui::GetMousePos(), min, size);
    if (state.lastFrame != frame - 1 && state.lastFrame != frame) {
        if (state.move.Active() || state.attribute.active) state.untilRelease = true;
        Cancel();
    }
    state.lastFrame = frame;
    const bool gesture = state.move.Active() || state.attribute.active;
    const bool cameraChanged = gesture && (!ctx.editorCamera
        || !SameMatrix(state.startView, ctx.editorCamera->GetViewMatrix())
        || !SameMatrix(state.startProjection, ctx.editorCamera->GetProjectionMatrix())
        || state.startMin.x != min.x || state.startMin.y != min.y || state.startSize.x != size.x || state.startSize.y != size.y);
    const bool authoringAllowed = IsControlPointAuthoringAllowed(ctx);
    const bool cancelInput = !editable || !authoringAllowed || blocked || !ctx.editorCamera || cameraChanged
        || ImGui::IsKeyPressed(ImGuiKey_Escape)
        || ((state.move.Active() || state.wasUsing)
            && ctx.gizmoMode != EditorContext::GizmoMode::Translate)
        || (state.move.Active() && !state.move.Validate(ctx));
    if (cancelInput) {
        if (state.move.Active() || state.attribute.active || state.wasUsing) {
            if (ImGuizmo::IsUsing()) { ImGuizmo::Enable(false); ImGuizmo::Enable(true); }
            state.untilRelease = true;
        }
        Cancel();
    }
    if (!editable || !ctx.editorCamera || !ctx.activeScene)
        return state.untilRelease && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    bool suppressGesture = false;
    if (state.untilRelease) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) state.untilRelease = false;
        else suppressGesture = true;
    }
    const bool otherClaim = ctx.controlPointMouseClaimFrame == frame && !state.move.Active() && !state.attribute.active;
    /// @note HUDや別ツールのmouse取得時は入力だけ止め、Scene Viewの点表示は維持する。
    const bool canStart = !cancelInput && !suppressGesture && authoringAllowed && hovered && inside
        && !otherClaim && !ImGui::GetIO().KeyAlt
        && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)
        && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const auto viewProjection = ctx.editorCamera->GetProjectionMatrix() * ctx.editorCamera->GetViewMatrix();
    if (state.selection != ctx.selectedEntities) {
        state.selection = ctx.selectedEntities; state.selectedIds.clear();
        for (const auto entity : state.selection) state.selectedIds.insert(static_cast<uint64_t>(entity.generation) << 32 | entity.index);
    }
    bool claimed = state.move.Active() || state.attribute.active || suppressGesture;
    auto* owner = EditOwner(ctx);
    auto* water = owner ? owner->GetComponent<scene::WaterComponent>() : nullptr;
    auto* spline = owner ? owner->GetComponent<scene::SplineComponent>() : nullptr;
    state.curve.clear(); state.polylines.clear(); state.handles.clear(); state.ownerMarkers.clear();
    const auto addOwnerMarker = [&](const scene::ObjectReference& reference,
        const math::Vector3& base, const math::Vector3& offset) {
        const auto entity = BindingTarget(*ctx.activeScene, reference);
        if (entity.IsValid()) state.ownerMarkers.push_back({entity, base, offset});
    };
    if (water && water->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS) {
        auto points = scene::ResolveWaterControlPointAuthoring(*ctx.activeScene, owner->GetID(), *water, water->pointRuntimeMode);
        if (points.valid) {
            for (size_t index = 0; index < points.points.size(); ++index) {
                auto point = points.points[index];
                const auto& binding = water->pointBindings[index];
                addOwnerMarker(binding.target, owner->transform.worldPosition,
                    Offset(owner->transform, point.localPosition));
                if (const auto* preview = PreviewFor(state.move, binding.target.id)) {
                    const auto local = owner->transform.worldRotation.Inverse() * preview->worldDelta;
                    point.localPosition += math::Vector3{local.x / owner->transform.worldScale.x, local.y / owner->transform.worldScale.y, local.z / owner->transform.worldScale.z};
                }
                state.curve.push_back(point.localPosition);
                if (state.move.Active()) continue;
                const auto base = owner->transform.worldPosition;
                const auto pointOffset = Offset(owner->transform, point.localPosition);
                const float depth = state.attribute.active && state.attribute.handle.id == point.id && !state.attribute.handle.width ? state.attribute.candidate : point.depth;
                WaterHandle handle{point.id, false, depth, base, pointOffset, {0.0f, -1.0f, 0.0f}};
                if (Project(viewProjection, min, size, owner->transform.worldPosition, Offset(owner->transform, point.localPosition) + handle.axis * depth, handle.screen)) state.handles.push_back(handle);
                if (water->surfaceShape == scene::WaterSurfaceShape::SPLINE_RIVER && points.points.size() > 1) {
                    const auto prior = points.points[index > 0 ? index - 1 : index].localPosition;
                    const auto next = points.points[index + 1 < points.points.size() ? index + 1 : index].localPosition;
                    const math::Vector3 normal{-(next.z - prior.z), 0.0f, next.x - prior.x};
                    if (normal.LengthSq() > 1e-8f) {
                        const auto axis = Offset(owner->transform, normal.Normalized()) * 0.5f;
                        const float width = state.attribute.active && state.attribute.handle.id == point.id && state.attribute.handle.width ? state.attribute.candidate : point.width;
                        for (const float sign : {-1.0f, 1.0f}) {
                            WaterHandle widthHandle{point.id, true, width, base, pointOffset, axis * sign};
                            if (Project(viewProjection, min, size, owner->transform.worldPosition, Offset(owner->transform, point.localPosition) + widthHandle.axis * width, widthHandle.screen)) state.handles.push_back(widthHandle);
                        }
                    }
                }
            }
        }
    } else if (water && water->pointSourceMode == scene::ControlPointSourceMode::INLINE
        && water->surfaceShape != scene::WaterSurfaceShape::RECTANGLE) {
        for (const auto& point : water->splinePoints) state.curve.push_back(point.localPosition);
    } else if (spline && spline->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS) {
        const auto points = scene::ResolveSplinePointAuthoring(*ctx.activeScene, owner->GetID(), *spline, spline->pointRuntimeMode);
        if (points.valid) for (size_t index = 0; index < points.points.size(); ++index) {
            auto localPosition = points.points[index].localPosition;
            addOwnerMarker(spline->pointBindings[index].target, owner->transform.worldPosition,
                Offset(owner->transform, localPosition));
            if (const auto* preview = PreviewFor(state.move, spline->pointBindings[index].target.id)) {
                const auto local = owner->transform.worldRotation.Inverse() * preview->worldDelta;
                localPosition += math::Vector3{local.x / owner->transform.worldScale.x, local.y / owner->transform.worldScale.y, local.z / owner->transform.worldScale.z};
            }
            state.curve.push_back(localPosition);
        }
    }
    if (owner) {
        if (auto* line = owner->GetComponent<scene::LineRendererComponent>()) {
            auto numeric = *line;
            const auto points = scene::ResolveLineControlPointAuthoring(*ctx.activeScene, owner->GetID(),
                numeric, line->pointRuntimeMode);
            if (points.valid) {
                WorldPolyline path;
                path.closed = line->loop;
                path.points.reserve(points.points.size());
                const auto* ownerPreview = PreviewFor(state.move, owner->GetID());
                for (size_t index = 0; index < points.points.size(); ++index) {
                    const auto point = points.points[index];
                    auto worldPoint = points.space == scene::LineSpace::Local
                        ? owner->transform.worldPosition + Offset(owner->transform, point) : point;
                    if (line->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS
                        && index < line->pointBindings.size()) {
                        addOwnerMarker(line->pointBindings[index].target,
                            points.space == scene::LineSpace::Local ? owner->transform.worldPosition : point,
                            points.space == scene::LineSpace::Local ? Offset(owner->transform, point) : math::Vector3::ZERO);
                        const auto target = BindingTarget(*ctx.activeScene, line->pointBindings[index].target);
                        if (const auto* preview = PreviewFor(state.move, target)) worldPoint += preview->worldDelta;
                    } else if (points.space == scene::LineSpace::Local && ownerPreview) {
                        worldPoint += ownerPreview->worldDelta;
                    }
                    path.points.push_back(worldPoint);
                }
                if (path.points.size() >= 2) state.polylines.push_back(std::move(path));
            }
        }
        if (auto* patrol = owner->GetComponent<scene::NavMeshPatrolComponent>()) {
            const auto points = scene::ResolveNavMeshPatrolPointAuthoring(*ctx.activeScene, owner->GetID(),
                *patrol, patrol->pointRuntimeMode);
            if (points.valid) {
                WorldPolyline path;
                path.closed = patrol->mode == scene::NavMeshPatrolComponent::Mode::LOOP;
                path.points.reserve(points.points.size());
                for (size_t index = 0; index < points.points.size(); ++index) {
                    auto point = points.points[index].worldPosition;
                    if (patrol->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS
                        && index < patrol->waypointBindings.size()) {
                        addOwnerMarker(patrol->waypointBindings[index].target, point, math::Vector3::ZERO);
                        const auto target = BindingTarget(*ctx.activeScene, patrol->waypointBindings[index].target);
                        if (const auto* preview = PreviewFor(state.move, target)) point += preview->worldDelta;
                    }
                    path.points.push_back(point);
                }
                if (path.points.size() >= 2) state.polylines.push_back(std::move(path));
            }
        }
        if (const auto* link = owner->GetComponent<scene::NavMeshOffMeshLinkComponent>()) {
            const auto endpoints = scene::ResolveNavMeshOffMeshLinkEndpoints(*ctx.activeScene, owner->GetID(), *link);
            if (endpoints.valid) {
                WorldPolyline path;
                path.points = {endpoints.startPoint, endpoints.endPoint};
                if (link->pointSourceMode == scene::ControlPointSourceMode::POINT_OBJECTS) {
                    addOwnerMarker(link->startTarget, endpoints.startPoint, math::Vector3::ZERO);
                    addOwnerMarker(link->endTarget, endpoints.endPoint, math::Vector3::ZERO);
                    if (const auto* preview = PreviewFor(state.move, BindingTarget(*ctx.activeScene, link->startTarget)))
                        path.points[0] += preview->worldDelta;
                    if (const auto* preview = PreviewFor(state.move, BindingTarget(*ctx.activeScene, link->endTarget)))
                        path.points[1] += preview->worldDelta;
                }
                state.polylines.push_back(std::move(path));
            }
        }
    }
    std::sort(state.ownerMarkers.begin(), state.ownerMarkers.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.entity.index < rhs.entity.index; });
    auto* draw = ImGui::GetWindowDrawList();
    if (owner && state.curve.size() >= 2) {
        const bool closed = spline ? spline->closed : water->surfaceShape == scene::WaterSurfaceShape::SPLINE_LAKE;
        const size_t samples = water ? std::clamp(static_cast<size_t>(water->splineSamplesPerSegment), size_t{1}, size_t{64}) : size_t{12};
        const size_t segments = (std::min)(size_t{2048}, state.curve.size() * samples);
        for (size_t index = 0; index < segments; ++index) {
            const auto a = scene::EvaluateSplineCurve(state.curve, closed, static_cast<float>(index) / static_cast<float>(segments));
            const auto b = scene::EvaluateSplineCurve(state.curve, closed, static_cast<float>(index + 1) / static_cast<float>(segments));
            ImVec2 start, end;
            if (Project(viewProjection, min, size, owner->transform.worldPosition, Offset(owner->transform, a), start)
                && Project(viewProjection, min, size, owner->transform.worldPosition, Offset(owner->transform, b), end))
                draw->AddLine(start, end, IM_COL32(90, 210, 255, 220), 2.0f);
        }
    }
    for (const auto& path : state.polylines) {
        for (size_t index = 1; index < path.points.size(); ++index) {
            ImVec2 start, end;
            if (Project(viewProjection, min, size, path.points[index - 1], math::Vector3::ZERO, start)
                && Project(viewProjection, min, size, path.points[index], math::Vector3::ZERO, end))
                draw->AddLine(start, end, IM_COL32(255, 170, 90, 220), 2.0f);
        }
        if (path.closed && path.points.size() >= 3) {
            ImVec2 start, end;
            if (Project(viewProjection, min, size, path.points.back(), math::Vector3::ZERO, start)
                && Project(viewProjection, min, size, path.points.front(), math::Vector3::ZERO, end))
                draw->AddLine(start, end, IM_COL32(255, 170, 90, 220), 2.0f);
        }
    }
    scene::EntityID hit{};
    std::size_t inlineHit = std::numeric_limits<std::size_t>::max();
    float best = std::numeric_limits<float>::max();
    size_t labels = 0;
    constexpr size_t MAX_POINT_LABELS = 32;
    draw->PushClipRect(min, {min.x + size.x, min.y + size.y}, true);
    if (water && water->pointSourceMode == scene::ControlPointSourceMode::INLINE
        && water->surfaceShape != scene::WaterSurfaceShape::RECTANGLE) {
        for (std::size_t index = 0; index < water->splinePoints.size(); ++index) {
            ImVec2 screen;
            if (!Project(viewProjection, min, size, owner->transform.worldPosition,
                    Offset(owner->transform, water->splinePoints[index].localPosition), screen)) continue;
            const auto mouse = ImGui::GetMousePos();
            const float dx = mouse.x - screen.x, dy = mouse.y - screen.y;
            const float distance = dx * dx + dy * dy;
            if (canStart && !ctx.IsLocked(owner->GetID()) && distance <= 100.0f && distance < best) {
                best = distance; inlineHit = index;
            }
            draw->AddCircleFilled(screen, 5.0f, IM_COL32(90, 210, 255, 240));
            draw->AddCircle(screen, 7.0f, IM_COL32(20, 35, 45, 255), 16, 1.5f);
            if (index < MAX_POINT_LABELS) {
                const auto label = std::to_string(index + 1);
                draw->AddText({screen.x + 10.0f, screen.y - 6.0f}, IM_COL32(210, 240, 255, 255), label.c_str());
            }
        }
    }
    for (const auto id : ctx.activeScene->GetEntities<scene::TargetPointComponent>()) {
        auto* target = ctx.activeScene->GetGameObject(id);
        if (!target) continue;
        auto& object = *target;
        const auto* marker = object.GetComponent<scene::TargetPointComponent>();
        if (!marker || !marker->visible) continue;
        const auto* preview = PreviewFor(state.move, object.GetID());
        const auto* placement = PlacementFor(state.ownerMarkers, object.GetID());
        ImVec2 screen;
        /// @note 所有者の数値点はbaseと相対offsetを分け、大きなworld座標で丸められたGO位置へ戻さない。
        const auto base = placement ? placement->base : object.transform.worldPosition;
        const auto offset = (placement ? placement->offset : math::Vector3::ZERO)
            + (preview ? preview->worldDelta : math::Vector3::ZERO);
        if (!Project(viewProjection, min, size, base, offset, screen)) continue;
        const float radius = std::isfinite(marker->screenSize) ? std::clamp(marker->screenSize, 4.0f, 30.0f) : 9.0f;
        const auto entity = object.GetID();
        const bool selected = state.selectedIds.contains(static_cast<uint64_t>(entity.generation) << 32 | entity.index);
        const auto& color = marker->color;
        const auto colorByte = [](float value) { return static_cast<int>(std::clamp(std::isfinite(value) ? value : 1.0f, 0.0f, 1.0f) * 255.0f); };
        const ImU32 tint = IM_COL32(colorByte(color.x), colorByte(color.y), colorByte(color.z), object.activeInHierarchy() ? colorByte(color.w) : 100);
        const auto mouse = ImGui::GetMousePos();
        const float distance = (mouse.x - screen.x) * (mouse.x - screen.x) + (mouse.y - screen.y) * (mouse.y - screen.y);
        const bool pointHovered = canStart && distance <= (radius + 3.0f) * (radius + 3.0f);
        draw->AddCircleFilled(screen, radius * 0.65f, tint, 12);
        const ImU32 outline = selected ? IM_COL32(255, 225, 80, 255)
            : pointHovered ? IM_COL32(255, 255, 255, 255) : tint;
        draw->AddCircle(screen, pointHovered ? radius + 2.0f : radius, outline, 16,
            selected || pointHovered ? 2.5f : 1.5f);
        if ((selected || pointHovered) && labels < MAX_POINT_LABELS) {
            const auto textSize = ImGui::CalcTextSize(object.name.c_str());
            const ImVec2 textMin{screen.x + radius + 6.0f, screen.y - textSize.y * 0.5f};
            draw->AddRectFilled({textMin.x - 3.0f, textMin.y - 2.0f},
                {textMin.x + textSize.x + 3.0f, textMin.y + textSize.y + 2.0f}, IM_COL32(20, 24, 30, 210), 3.0f);
            draw->AddText(textMin, selected ? IM_COL32(255, 225, 80, 255) : IM_COL32(255, 255, 255, 255),
                object.name.c_str());
            ++labels;
        }
        if (!ctx.IsLocked(object.GetID()) && distance <= (radius + 3.0f) * (radius + 3.0f) && distance < best) { best = distance; hit = object.GetID(); }
    }
    draw->PopClipRect();
    for (const auto& handle : state.handles) {
        draw->AddRectFilled({handle.screen.x - 4, handle.screen.y - 4}, {handle.screen.x + 4, handle.screen.y + 4},
            handle.width ? IM_COL32(100, 255, 170, 245) : IM_COL32(120, 165, 255, 245));
        ImVec2 start;
        if (Project(viewProjection, min, size, handle.base, handle.offset, start)) draw->AddLine(start, handle.screen, IM_COL32(130, 195, 255, 160));
    }
    if (state.attribute.active) {
        auto& drag = state.attribute;
        auto* currentOwner = ctx.activeScene->GetGameObject(drag.owner);
        auto* currentWater = currentOwner ? currentOwner->GetComponent<scene::WaterComponent>() : nullptr;
        const bool valid = currentWater && currentOwner->instanceId == drag.ownerGuid
            && drag.scene == ctx.activeScene->GetRenderSceneGeneration() && drag.revision == ctx.undoStack->GetRevision()
            && drag.lifetime == ctx.activeScene->GetComponentLifetime<scene::WaterComponent>(drag.owner)
            && drag.editOwner == ctx.controlPointOwnerGuid && SamePose(drag.pose, currentOwner->transform)
            && drag.selection == ctx.selectedEntities && drag.gizmoMode == static_cast<int>(ctx.gizmoMode)
            && drag.gizmoSpace == static_cast<int>(ctx.gizmoSpace) && drag.gizmoPivot == static_cast<int>(ctx.gizmoPivot)
            && drag.referenceSignature == ControlPointOwnerReferenceSignature(*ctx.activeScene, drag.owner)
            && scene::ResolveWaterControlPoints(*ctx.activeScene, drag.owner, *currentWater).inputSignature == drag.signature;
        if (!valid || !inside) { drag.active = false; state.untilRelease = true; return true; }
        float parameter = 0.0f;
        if (AxisParameter(ctx, min, size, drag.handle, parameter))
            drag.candidate = (std::max)(drag.handle.width ? 0.01f : 0.0f, drag.handle.value + parameter - drag.initialParameter);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            OpArgs args; args.Set("node", drag.ownerGuid); args.Set("kind", std::string("water")); args.Set("id", static_cast<int>(drag.handle.id));
            args.Set(drag.handle.width ? "width" : "depth", drag.candidate);
            static_cast<void>(InvokeOperator(ctx, "control_points.set_water_attributes", args)); drag.active = false;
        }
        ctx.controlPointMouseClaimFrame = frame;
        return true;
    }
    bool gizmoOver = false;
    const auto* primaryObject = ctx.GetSelectedGO();
    const bool finitePivot = primaryObject && std::isfinite(primaryObject->transform.worldPosition.x)
        && std::isfinite(primaryObject->transform.worldPosition.y) && std::isfinite(primaryObject->transform.worldPosition.z);
    if (ctx.gizmoMode == EditorContext::GizmoMode::Translate
        && HasTargetPointSelection(ctx) && finitePivot) {
        if (!state.move.Active()) {
            if (auto* primary = ctx.GetSelectedGO()) {
                state.origin = primary->transform.worldPosition;
                state.gizmoStartOffset = {};
                if (ctx.gizmoPivot == EditorContext::GizmoPivot::Center) {
                    float radius = 0.0f; static_cast<void>(ComputeSelectionBounds(ctx, state.origin, radius));
                }
                if (ctx.selectedEntities.size() == 1) {
                    if (const auto* placement = PlacementFor(state.ownerMarkers, primary->GetID())) {
                        state.origin = placement->base;
                        state.gizmoStartOffset = placement->offset;
                    }
                }
                state.gizmo = ctx.gizmoSpace == EditorContext::GizmoSpace::Local ? math::Matrix4::Rotate(primary->transform.worldRotation) : math::Matrix4::Identity();
                state.gizmo.m[0][3] = state.gizmoStartOffset.x;
                state.gizmo.m[1][3] = state.gizmoStartOffset.y;
                state.gizmo.m[2][3] = state.gizmoStartOffset.z;
            }
        }
        /// @note Gizmo原点を引いた座標で操作し、大きなworld位置へ小さなdeltaを加算してから差し引くことを避ける。
        /// @see https://github.com/CedricGuillemet/ImGuizmo Manipulate、ID stack。
        auto view = ToColumnMajor(ctx.editorCamera->GetViewMatrix() * math::Matrix4::Translate(state.origin));
        auto projection = ToColumnMajor(ctx.editorCamera->GetProjectionMatrix());
        auto gizmo = ToColumnMajor(state.gizmo);
        ImGuizmo::SetDrawlist(); ImGuizmo::SetOrthographic(ctx.editorCamera->m_projection == renderer::ProjectionMode::Orthographic);
        ImGuizmo::SetRect(min.x, min.y, size.x, size.y);
        const bool enable = !otherClaim && (!ImGuizmo::IsUsingAny() || ImGuizmo::IsUsing())
            && (state.move.Active() || canStart);
        const bool otherUsing = ImGuizmo::IsUsingAny() && !ImGuizmo::IsUsing();
        if (!otherUsing) ImGuizmo::Enable(enable);
        const float snap[] = {ctx.snapPos, ctx.snapPos, ctx.snapPos};
        if (!otherUsing)
            ImGuizmo::Manipulate(&view.m[0][0], &projection.m[0][0], ImGuizmo::TRANSLATE,
                ctx.gizmoSpace == EditorContext::GizmoSpace::Local ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
                &gizmo.m[0][0], nullptr, ctx.snapEnabled || ImGui::GetIO().KeyCtrl ? snap : nullptr);
        gizmoOver = enable && ImGuizmo::IsOver();
        const bool usingGizmo = enable && ImGuizmo::IsUsing();
        if (!otherUsing) ImGuizmo::Enable(true);
        state.gizmo = math::Matrix4::Transpose(gizmo);
        if (usingGizmo && !state.wasUsing) {
            if (!state.move.Begin(ctx)) { state.untilRelease = true; return true; }
            state.startView = ctx.editorCamera->GetViewMatrix(); state.startProjection = ctx.editorCamera->GetProjectionMatrix();
            state.startMin = min; state.startSize = size;
        }
        if (usingGizmo) {
            const math::Vector3 delta = math::Vector3{state.gizmo.m[0][3], state.gizmo.m[1][3], state.gizmo.m[2][3]}
                - state.gizmoStartOffset;
            if (!state.move.SetWorldDelta(ctx, delta)) { state.untilRelease = true; return true; }
            ctx.controlPointMouseClaimFrame = frame; claimed = true;
        } else if (state.wasUsing && state.move.Active()) {
            if (inside && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) static_cast<void>(state.move.Commit(ctx));
            else state.move.Cancel();
            claimed = true;
        }
        state.wasUsing = usingGizmo;
    } else if (state.move.Active()) Cancel();
    /// @note 移動Gizmoを描かないframeのIsOverは前のhoverを含むため、通常hoverは回転・拡縮の選択点だけに使う。
    const bool transformGizmoOwnsMouse = ImGuizmo::IsUsingAny()
        || (ctx.gizmoMode != EditorContext::GizmoMode::Translate
            && HasTargetPointSelection(ctx) && ImGuizmo::IsOver());
    if (canStart && hit.IsValid() && !gizmoOver && !transformGizmoOwnsMouse && !claimed) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (auto* point = ctx.activeScene->GetGameObject(hit))
            ImGui::SetTooltip("%s\nクリック: 選択 / Shift: 追加 / Ctrl: 切替", point->name.c_str());
    } else if (canStart && inlineHit != std::numeric_limits<std::size_t>::max()
        && !gizmoOver && !transformGizmoOwnsMouse && !claimed) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Water 点 %zu\nクリック: TargetPoint にして選択", inlineHit + 1);
    }
    if (canStart && !gizmoOver && !transformGizmoOwnsMouse && !claimed
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (!hit.IsValid() && inlineHit != std::numeric_limits<std::size_t>::max()) {
            /// @note Only an explicit click materializes numeric points; selection itself and Scene loading remain read-only.
            const std::string ownerGuid = owner->instanceId;
            OpArgs convert;
            convert.Set("node", ownerGuid); convert.Set("kind", std::string("water"));
            convert.Set("source", std::string("POINT_OBJECTS"));
            const auto result = InvokeOperator(ctx, "control_points.convert", convert);
            if (result.ok) {
                OpArgs edit; edit.Set("owner", ownerGuid);
                static_cast<void>(InvokeOperator(ctx, "control_points.edit", edit));
                const auto* targets = result.data.Find("targets");
                if (targets && targets->IsArray() && inlineHit < targets->AsArray().size()) {
                    OpArgs select; select.Set("target", targets->AsArray()[inlineHit].AsString());
                    static_cast<void>(InvokeOperator(ctx, "control_points.select", select));
                    ctx.gizmoMode = EditorContext::GizmoMode::Translate;
                }
            } else FBZZ_LOG_WARN("Water control point: %s", result.message.c_str());
            ctx.controlPointMouseClaimFrame = frame;
            return true;
        } else if (hit.IsValid()) {
            auto* point = ctx.activeScene->GetGameObject(hit);
            if (owner && point && OwnerReferencesPoint(*owner, point->instanceId)
                && ctx.controlPointOwnerGuid != owner->instanceId) {
                OpArgs edit; edit.Set("owner", owner->instanceId);
                static_cast<void>(InvokeOperator(ctx, "control_points.edit", edit));
            }
            if (ImGui::GetIO().KeyCtrl) ToggleSelection(ctx, hit);
            else if (ImGui::GetIO().KeyShift) AddToSelection(ctx, hit);
            else SelectEntity(ctx, hit);
            claimed = true;
        } else if (water && owner) {
            for (const auto& handle : state.handles) {
                const auto mouse = ImGui::GetMousePos();
                if (std::fabs(mouse.x - handle.screen.x) > 7.0f || std::fabs(mouse.y - handle.screen.y) > 7.0f) continue;
                float parameter = 0.0f;
                if (!AxisParameter(ctx, min, size, handle, parameter)) continue;
                auto& drag = state.attribute;
                drag.scene = ctx.activeScene->GetRenderSceneGeneration(); drag.owner = owner->GetID(); drag.ownerGuid = owner->instanceId;
                drag.lifetime = ctx.activeScene->GetComponentLifetime<scene::WaterComponent>(drag.owner);
                drag.revision = ctx.undoStack->GetRevision(); drag.pose = owner->transform; drag.editOwner = ctx.controlPointOwnerGuid;
                drag.selection = ctx.selectedEntities; drag.gizmoMode = static_cast<int>(ctx.gizmoMode);
                drag.gizmoSpace = static_cast<int>(ctx.gizmoSpace); drag.gizmoPivot = static_cast<int>(ctx.gizmoPivot);
                drag.referenceSignature = ControlPointOwnerReferenceSignature(*ctx.activeScene, drag.owner);
                drag.signature = scene::ResolveWaterControlPoints(*ctx.activeScene, drag.owner, *water).inputSignature;
                drag.handle = handle; drag.initialParameter = parameter; drag.candidate = handle.value; drag.active = true;
                state.startView = ctx.editorCamera->GetViewMatrix(); state.startProjection = ctx.editorCamera->GetProjectionMatrix();
                state.startMin = min; state.startSize = size;
                ctx.controlPointMouseClaimFrame = frame; claimed = true; break;
            }
        }
    }
    return claimed || gizmoOver;
}
} /// @note namespace fbzz::editor
