// FBZZ Engine
// ViewportPanel.cpp | fbzz::editor
// Viewport image, mouse picking, and ImGuizmo manipulation
#include <Editor/Panels/ViewportPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/IRenderTarget.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>
#include <imgui.h>
#include <ImGuizmo.h>
#include <algorithm>
#include <cmath>

namespace fbzz::editor {

namespace {

math::Matrix4 ToColumnMajor(const math::Matrix4& rowMajor)
{
    return math::Matrix4::Transpose(rowMajor);
}

math::Vector3 ScreenRayFromMouse(const EditorContext& ctx, const ImVec2& viewportMin)
{
    ImVec2 mouse = ImGui::GetMousePos();
    float vx = mouse.x - viewportMin.x;
    float vy = mouse.y - viewportMin.y;

    float nx = (vx / ctx.viewportWidth) * 2.0f - 1.0f;
    float ny = 1.0f - (vy / ctx.viewportHeight) * 2.0f;

    const math::Matrix4 view = ctx.editorCamera->GetViewMatrix();
    const math::Matrix4 proj = ctx.editorCamera->GetProjectionMatrix();
    const math::Matrix4 invVP = math::Matrix4::Inverse(proj * view);

    math::Vector4 nearClip = { nx, ny, 0.0f, 1.0f };
    math::Vector4 farClip = { nx, ny, 1.0f, 1.0f };

    math::Vector4 nearWorld4 = invVP * nearClip;
    math::Vector4 farWorld4 = invVP * farClip;
    if (!math::NearlyZero(nearWorld4.w)) nearWorld4 = nearWorld4 * (1.0f / nearWorld4.w);
    if (!math::NearlyZero(farWorld4.w)) farWorld4 = farWorld4 * (1.0f / farWorld4.w);

    math::Vector3 nearWorld = { nearWorld4.x, nearWorld4.y, nearWorld4.z };
    math::Vector3 farWorld = { farWorld4.x, farWorld4.y, farWorld4.z };
    return (farWorld - nearWorld).Normalized();
}

void HandleGizmoShortcuts(EditorContext& ctx)
{
    if (input::Input::KeyDown(input::KeyCode::W)) ctx.gizmoMode = EditorContext::GizmoMode::Translate;
    if (input::Input::KeyDown(input::KeyCode::E)) ctx.gizmoMode = EditorContext::GizmoMode::Rotate;
    if (input::Input::KeyDown(input::KeyCode::R)) ctx.gizmoMode = EditorContext::GizmoMode::Scale;
    if (input::Input::KeyDown(input::KeyCode::Q)) {
        ctx.gizmoSpace = (ctx.gizmoSpace == EditorContext::GizmoSpace::World)
            ? EditorContext::GizmoSpace::Local
            : EditorContext::GizmoSpace::World;
    }
}

bool PickEntity(EditorContext& ctx, const ImVec2& viewportMin)
{
    if (!ctx.activeScene || !ctx.editorCamera) return false;

    const math::Vector3 rayOrigin = ctx.editorCamera->m_position;
    const math::Vector3 rayDir = ScreenRayFromMouse(ctx, viewportMin);
    ImVec2 mouse = ImGui::GetMousePos();
    FBZZ_LOG_INFO("Pick start: mouse=(%.1f, %.1f) vpMin=(%.1f, %.1f) vpSize=(%.1f, %.1f) rayO=(%.3f, %.3f, %.3f) rayD=(%.3f, %.3f, %.3f)",
                  mouse.x, mouse.y, viewportMin.x, viewportMin.y,
                  ctx.viewportWidth, ctx.viewportHeight,
                  rayOrigin.x, rayOrigin.y, rayOrigin.z,
                  rayDir.x, rayDir.y, rayDir.z);

    scene::EntityID best = scene::EntityID::INVALID;
    float bestT = 1e30f;
    scene::EntityID bestFallback = scene::EntityID::INVALID;
    float bestFallbackT = 1e30f;

    auto transformPoint = [](const math::Matrix4& m, const math::Vector3& p) {
        math::Vector4 v = m * math::Vector4{ p.x, p.y, p.z, 1.0f };
        if (!math::NearlyZero(v.w)) v = v * (1.0f / v.w);
        return math::Vector3{ v.x, v.y, v.z };
    };
    auto rayTriangle = [](const math::Vector3& ro, const math::Vector3& rd,
                          const math::Vector3& v0, const math::Vector3& v1, const math::Vector3& v2,
                          float& outT) {
        const float kEps = 1e-6f;
        math::Vector3 e1 = v1 - v0;
        math::Vector3 e2 = v2 - v0;
        math::Vector3 p = math::Vector3::Cross(rd, e2);
        float det = math::Vector3::Dot(e1, p);
        if (det > -kEps && det < kEps) return false;
        float invDet = 1.0f / det;
        math::Vector3 t = ro - v0;
        float u = math::Vector3::Dot(t, p) * invDet;
        if (u < 0.0f || u > 1.0f) return false;
        math::Vector3 q = math::Vector3::Cross(t, e1);
        float v = math::Vector3::Dot(rd, q) * invDet;
        if (v < 0.0f || (u + v) > 1.0f) return false;
        float hitT = math::Vector3::Dot(e2, q) * invDet;
        if (hitT <= kEps) return false;
        outT = hitT;
        return true;
    };

    for (auto& go : ctx.activeScene->GameObjects()) {
        auto* mr = go.GetComponent<scene::MeshRenderer>();
        if (!mr || !mr->mesh) continue;
        const auto& verts = mr->mesh->cpuVertices;
        const auto& indices = mr->mesh->cpuIndices;
        if (verts.empty() || indices.size() < 3) continue;

        const math::Matrix4 world = go.transform.GetWorldMatrix();
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const uint32_t i0 = indices[i + 0];
            const uint32_t i1 = indices[i + 1];
            const uint32_t i2 = indices[i + 2];
            if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) continue;

            const math::Vector3 v0 = transformPoint(world, verts[i0].position);
            const math::Vector3 v1 = transformPoint(world, verts[i1].position);
            const math::Vector3 v2 = transformPoint(world, verts[i2].position);

            float t = 0.0f;
            if (rayTriangle(rayOrigin, rayDir, v0, v1, v2, t) && t < bestT) {
                bestT = t;
                best = go.GetID();
            }
        }

        const math::Vector3 center = go.transform.position;
        const float radius = (std::max)(0.5f, go.transform.worldScale.Length() / 3.0f);
        math::Vector3 oc = rayOrigin - center;
        float a = math::Vector3::Dot(rayDir, rayDir);
        float b = 2.0f * math::Vector3::Dot(oc, rayDir);
        float c = math::Vector3::Dot(oc, oc) - radius * radius;
        float disc = b * b - 4.0f * a * c;
        if (disc >= 0.0f) {
            float s = std::sqrt(disc);
            float t0 = (-b - s) / (2.0f * a);
            float t1 = (-b + s) / (2.0f * a);
            float t = (t0 > 0.0f) ? t0 : ((t1 > 0.0f) ? t1 : -1.0f);
            if (t > 0.0f && t < bestFallbackT) {
                bestFallbackT = t;
                bestFallback = go.GetID();
            }
        }
    }

    if (!best.IsValid() && bestFallback.IsValid()) {
        best = bestFallback;
        bestT = bestFallbackT;
    }

    if (best.IsValid()) {
        if (!ImGui::GetIO().KeyCtrl) ctx.selectedEntities.clear();
        if (std::find(ctx.selectedEntities.begin(), ctx.selectedEntities.end(), best) == ctx.selectedEntities.end())
            ctx.selectedEntities.push_back(best);
        FBZZ_LOG_INFO("Pick hit: index=%u gen=%u selectedCount=%zu",
                      best.index, best.generation, ctx.selectedEntities.size());
        return true;
    }

    if (!ImGui::GetIO().KeyCtrl) ctx.selectedEntities.clear();
    FBZZ_LOG_INFO("Pick miss: selection cleared=%s selectedCount=%zu",
                  ImGui::GetIO().KeyCtrl ? "false" : "true",
                  ctx.selectedEntities.size());
    return false;
}

void DrawGizmo(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize)
{
    scene::EntityID selected = ctx.PrimarySelected();
    if (!selected.IsValid() || !ctx.activeScene || !ctx.editorCamera) return;

    scene::GameObject* go = ctx.activeScene->GetGameObject(selected);
    if (!go) return;

    HandleGizmoShortcuts(ctx);

    math::Matrix4 viewCol = ToColumnMajor(ctx.editorCamera->GetViewMatrix());
    math::Matrix4 projCol = ToColumnMajor(ctx.editorCamera->GetProjectionMatrix());
    math::Matrix4 worldCol = ToColumnMajor(go->transform.GetWorldMatrix());

    auto& style = ImGuizmo::GetStyle();
    style.Colors[ImGuizmo::SELECTION] = ImVec4(1.0f, 0.95f, 0.05f, 1.0f);
    style.Colors[ImGuizmo::INACTIVE] = ImVec4(0.75f, 0.75f, 0.75f, 0.75f);
    style.TranslationLineThickness = 4.0f;
    style.RotationLineThickness = 4.0f;
    style.ScaleLineThickness = 4.0f;

    ImGuizmo::SetDrawlist();
    ImGuizmo::Enable(true);
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(viewportMin.x, viewportMin.y, viewportSize.x, viewportSize.y);

    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    if (ctx.gizmoMode == EditorContext::GizmoMode::Rotate) op = ImGuizmo::ROTATE;
    if (ctx.gizmoMode == EditorContext::GizmoMode::Scale) op = ImGuizmo::SCALE;

    ImGuizmo::MODE mode = (ctx.gizmoSpace == EditorContext::GizmoSpace::World)
        ? ImGuizmo::WORLD
        : ImGuizmo::LOCAL;

    float snap[3] = { ctx.snapDistance, ctx.snapDistance, ctx.snapDistance };
    static int s_lastOp = -1;
    static int s_lastMode = -1;
    const int opInt = static_cast<int>(op);
    const int modeInt = static_cast<int>(mode);
    if (s_lastOp != opInt || s_lastMode != modeInt) {
        s_lastOp = opInt;
        s_lastMode = modeInt;
        FBZZ_LOG_INFO("Gizmo mode changed: op=%d mode=%d snap=%s snapDist=%.3f",
                      opInt, modeInt, ctx.snapEnabled ? "on" : "off", ctx.snapDistance);
    }

    ImGuizmo::Manipulate(
        &viewCol.m[0][0],
        &projCol.m[0][0],
        op,
        mode,
        &worldCol.m[0][0],
        nullptr,
        ctx.snapEnabled ? snap : nullptr);

    const bool gizmoOver = ImGuizmo::IsOver();
    const bool gizmoUsing = ImGuizmo::IsUsing();
    static bool s_prevOver = false;
    static bool s_prevUsing = false;
    if (gizmoOver != s_prevOver || gizmoUsing != s_prevUsing) {
        s_prevOver = gizmoOver;
        s_prevUsing = gizmoUsing;
        FBZZ_LOG_INFO("Gizmo state: over=%s using=%s selected=(%u,%u)",
                      gizmoOver ? "true" : "false",
                      gizmoUsing ? "true" : "false",
                      selected.index, selected.generation);
    }

    if (!gizmoUsing) return;

    math::Matrix4 worldRow = math::Matrix4::Transpose(worldCol);
    math::Matrix4 localRow = worldRow;
    if (scene::GameObject* parent = go->GetParent()) {
        const math::Matrix4 parentInv = math::Matrix4::Inverse(parent->transform.GetWorldMatrix());
        localRow = parentInv * worldRow;
    }

    const math::Matrix4 localCol = math::Matrix4::Transpose(localRow);
    float m[16] = {
        localCol.m[0][0], localCol.m[0][1], localCol.m[0][2], localCol.m[0][3],
        localCol.m[1][0], localCol.m[1][1], localCol.m[1][2], localCol.m[1][3],
        localCol.m[2][0], localCol.m[2][1], localCol.m[2][2], localCol.m[2][3],
        localCol.m[3][0], localCol.m[3][1], localCol.m[3][2], localCol.m[3][3]
    };

    float t[3] = {};
    float r[3] = {};
    float s[3] = {};
    ImGuizmo::DecomposeMatrixToComponents(m, t, r, s);

    go->transform.localPosition = { t[0], t[1], t[2] };
    go->transform.localScale = { s[0], s[1], s[2] };
    go->transform.localRotation = math::Quaternion::FromEuler({
        r[0] * math::DEG2RAD,
        r[1] * math::DEG2RAD,
        r[2] * math::DEG2RAD
    });
    FBZZ_LOG_INFO("Gizmo applied: id=(%u,%u) localPos=(%.3f, %.3f, %.3f) localRotDeg=(%.3f, %.3f, %.3f) localScale=(%.3f, %.3f, %.3f)",
                  selected.index, selected.generation,
                  t[0], t[1], t[2], r[0], r[1], r[2], s[0], s[1], s[2]);
}

} // namespace

void ViewportPanel::OnBeforeBegin(EditorContext& /*ctx*/)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, 0.0f });
}

void ViewportPanel::OnAfterBegin(EditorContext& /*ctx*/)
{
    ImGui::PopStyleVar();
}

void ViewportPanel::OnRenderContent(EditorContext& ctx)
{
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 1.0f) size.x = 1.0f;
    if (size.y < 1.0f) size.y = 1.0f;

    ctx.viewportWidth = size.x;
    ctx.viewportHeight = size.y;
    ctx.viewportFocused = ImGui::IsWindowFocused();

    ImVec2 viewportMin = ImGui::GetCursorScreenPos();
    ImVec2 viewportMax = { viewportMin.x + size.x, viewportMin.y + size.y };
    bool viewportHovered = ImGui::IsMouseHoveringRect(viewportMin, viewportMax);
    if (hdrRT) {
        ImTextureID texID = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(hdrRT->GetNativeSRV(0)));
        ImGui::GetWindowDrawList()->AddImage(texID, viewportMin, viewportMax);
    } else {
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            cursor, { cursor.x + size.x, cursor.y + size.y }, IM_COL32(30, 30, 30, 255));
        ImGui::SetCursorScreenPos({ cursor.x + size.x * 0.5f - 60.0f, cursor.y + size.y * 0.5f - 7.0f });
        ImGui::TextDisabled("No Render Target");
    }

    static bool s_prevHovered = false;
    if (viewportHovered != s_prevHovered) {
        s_prevHovered = viewportHovered;
        FBZZ_LOG_INFO("Viewport hover: %s", viewportHovered ? "true" : "false");
    }
    const bool gizmoWantsMouse = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
    if (viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !gizmoWantsMouse)
        PickEntity(ctx, viewportMin);

    DrawGizmo(ctx, viewportMin, size);

    if (ctx.showSceneStats) {
        int entityCount = 0;
        int meshCount = 0;
        if (ctx.activeScene) {
            for ([[maybe_unused]] auto& go : ctx.activeScene->GameObjects()) ++entityCount;
            meshCount = static_cast<int>(ctx.activeScene->GetEntities<scene::MeshRenderer>().size());
        }

        ImVec2 winPos = ImGui::GetWindowPos();
        ImVec2 winSize = ImGui::GetWindowSize();
        ImGui::SetNextWindowPos({ winPos.x + winSize.x - 8.0f, winPos.y + 8.0f }, ImGuiCond_Always, { 1.0f, 0.0f });
        ImGui::SetNextWindowBgAlpha(0.55f);
        constexpr ImGuiWindowFlags kOverlayFlags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoFocusOnAppearing;
        if (ImGui::Begin("##vp_stats", nullptr, kOverlayFlags)) {
            ImGui::Text("FPS      %.1f", ImGui::GetIO().Framerate);
            ImGui::Text("Entities %d", entityCount);
            ImGui::Text("Meshes   %d", meshCount);
        }
        ImGui::End();
    }

}

} // namespace fbzz::editor
