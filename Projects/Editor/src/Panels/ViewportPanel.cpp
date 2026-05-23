// FBZZ Engine
// ViewportPanel.cpp | fbzz::editor
// Viewport image, mouse picking, and ImGuizmo manipulation
#include <Editor/Panels/ViewportPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>
#include <Math/Ray.hpp>
#include <imgui.h>
#include <ImGuizmo.h>
#include <algorithm>

namespace fbzz::editor {

namespace {

math::Matrix4 ToColumnMajor(const math::Matrix4& rowMajor)
{
    return math::Matrix4::Transpose(rowMajor);
}

math::Ray ScreenRayFromMouse(const EditorContext& ctx, const ImVec2& viewportMin)
{
    ImVec2 mouse = ImGui::GetMousePos();
    float nx = ((mouse.x - viewportMin.x) / ctx.viewportWidth)  * 2.0f - 1.0f;
    float ny = 1.0f - ((mouse.y - viewportMin.y) / ctx.viewportHeight) * 2.0f;

    const math::Matrix4 invVP = math::Matrix4::Inverse(
        ctx.editorCamera->GetProjectionMatrix() * ctx.editorCamera->GetViewMatrix());
    return math::Ray::FromNDC(nx, ny, ctx.editorCamera->m_position, invVP);
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

    const math::Ray ray = ScreenRayFromMouse(ctx, viewportMin);

    scene::EntityID best         = scene::EntityID::INVALID;
    float           bestT        = 1e30f;
    scene::EntityID bestFallback = scene::EntityID::INVALID;
    float           bestFallbackT = 1e30f;

    auto transformPoint = [](const math::Matrix4& m, const math::Vector3& p) {
        math::Vector4 v = m * math::Vector4{ p.x, p.y, p.z, 1.0f };
        if (!math::NearlyZero(v.w)) v = v * (1.0f / v.w);
        return math::Vector3{ v.x, v.y, v.z };
    };

    for (auto& go : ctx.activeScene->GameObjects()) {
        auto* mr = go.GetComponent<scene::MeshRenderer>();
        if (!mr || !mr->mesh) continue;
        const auto& verts   = mr->mesh->cpuVertices;
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
            if (ray.IntersectTriangle(v0, v1, v2, t) && t < bestT) {
                bestT = t;
                best  = go.GetID();
            }
        }

        const math::Vector3 center = go.transform.position;
        const float radius = (std::max)(0.5f, go.transform.worldScale.Length() / 3.0f);
        float t = 0.0f;
        if (ray.IntersectSphere(center, radius, t) && t < bestFallbackT) {
            bestFallbackT = t;
            bestFallback  = go.GetID();
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
        return true;
    }

    if (!ImGui::GetIO().KeyCtrl) ctx.selectedEntities.clear();
    return false;
}

void DrawOrientationGizmo(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize)
{
    if (!ctx.editorCamera) return;

    constexpr float kSize = 120.0f;
    const ImVec2 pos = { viewportMin.x + viewportSize.x - kSize - 8.0f, viewportMin.y + 8.0f };

    math::Matrix4 viewCol = ToColumnMajor(ctx.editorCamera->GetViewMatrix());

    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(viewportMin.x, viewportMin.y, viewportSize.x, viewportSize.y);

    const float dist = ctx.editorCamera->m_position.Length();
    ImGuizmo::ViewManipulate(
        &viewCol.m[0][0],
        (dist > 0.1f ? dist : 10.0f),
        pos, { kSize, kSize },
        0x40000000);

    if (!ImGuizmo::IsUsingViewManipulate()) return;

    // column-major → row-major に戻す
    const math::Matrix4 viewRow = math::Matrix4::Transpose(viewCol);

    // カメラ位置: pos = -R^T * t  (view の 3x3 = R^T, t = viewRow の平行移動列)
    const float tx = viewRow.m[0][3], ty = viewRow.m[1][3], tz = viewRow.m[2][3];
    ctx.editorCamera->m_position = {
        -(viewRow.m[0][0]*tx + viewRow.m[1][0]*ty + viewRow.m[2][0]*tz),
        -(viewRow.m[0][1]*tx + viewRow.m[1][1]*ty + viewRow.m[2][1]*tz),
        -(viewRow.m[0][2]*tx + viewRow.m[1][2]*ty + viewRow.m[2][2]*tz)
    };

    // カメラ回転: ワールド回転行列の列 = view の 3x3 の行 → 転置して FromMatrix4
    math::Matrix4 rotMat = math::Matrix4::Identity();
    rotMat.m[0][0] = viewRow.m[0][0]; rotMat.m[0][1] = viewRow.m[1][0]; rotMat.m[0][2] = viewRow.m[2][0];
    rotMat.m[1][0] = viewRow.m[0][1]; rotMat.m[1][1] = viewRow.m[1][1]; rotMat.m[1][2] = viewRow.m[2][1];
    rotMat.m[2][0] = viewRow.m[0][2]; rotMat.m[2][1] = viewRow.m[1][2]; rotMat.m[2][2] = viewRow.m[2][2];
    ctx.editorCamera->m_rotation = math::Quaternion::FromMatrix4(rotMat);
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
    }

    if (!gizmoUsing) return;

    math::Matrix4 worldRow = math::Matrix4::Transpose(worldCol);
    math::Matrix4 localRow = worldRow;
    if (scene::GameObject* parent = go->GetParent()) {
        const math::Matrix4 parentInv = math::Matrix4::Inverse(parent->transform.GetWorldMatrix());
        localRow = parentInv * worldRow;
    }

    // Extract TRS directly from localRow to avoid convention mismatch between
    // ImGuizmo's left-hand Euler decomposition and the engine's right-hand quaternion.
    const float sx = std::sqrt(localRow.m[0][0]*localRow.m[0][0] + localRow.m[1][0]*localRow.m[1][0] + localRow.m[2][0]*localRow.m[2][0]);
    const float sy = std::sqrt(localRow.m[0][1]*localRow.m[0][1] + localRow.m[1][1]*localRow.m[1][1] + localRow.m[2][1]*localRow.m[2][1]);
    const float sz = std::sqrt(localRow.m[0][2]*localRow.m[0][2] + localRow.m[1][2]*localRow.m[1][2] + localRow.m[2][2]*localRow.m[2][2]);

    math::Matrix4 rotMat = math::Matrix4::Identity();
    if (!math::NearlyZero(sx)) { rotMat.m[0][0] = localRow.m[0][0]/sx; rotMat.m[1][0] = localRow.m[1][0]/sx; rotMat.m[2][0] = localRow.m[2][0]/sx; }
    if (!math::NearlyZero(sy)) { rotMat.m[0][1] = localRow.m[0][1]/sy; rotMat.m[1][1] = localRow.m[1][1]/sy; rotMat.m[2][1] = localRow.m[2][1]/sy; }
    if (!math::NearlyZero(sz)) { rotMat.m[0][2] = localRow.m[0][2]/sz; rotMat.m[1][2] = localRow.m[1][2]/sz; rotMat.m[2][2] = localRow.m[2][2]/sz; }

    go->transform.localPosition = { localRow.m[0][3], localRow.m[1][3], localRow.m[2][3] };
    go->transform.localScale    = { sx, sy, sz };
    go->transform.localRotation = math::Quaternion::FromMatrix4(rotMat);
}

float GetGameViewportAspectRatio(EditorContext::GameViewportAspect aspect)
{
    switch (aspect) {
    case EditorContext::GameViewportAspect::Ratio16x9:  return 16.0f / 9.0f;
    case EditorContext::GameViewportAspect::Ratio4x3:   return 4.0f / 3.0f;
    case EditorContext::GameViewportAspect::Ratio1x1:   return 1.0f;
    case EditorContext::GameViewportAspect::Ratio9x16:  return 9.0f / 16.0f;
    case EditorContext::GameViewportAspect::HD:          return 1280.0f / 720.0f;
    case EditorContext::GameViewportAspect::FullHD:      return 1920.0f / 1080.0f;
    case EditorContext::GameViewportAspect::QHD:         return 2560.0f / 1440.0f;
    case EditorContext::GameViewportAspect::UHD4K:       return 3840.0f / 2160.0f;
    case EditorContext::GameViewportAspect::WXGA:        return 1280.0f / 800.0f;
    case EditorContext::GameViewportAspect::WUXGA:       return 1920.0f / 1200.0f;
    case EditorContext::GameViewportAspect::iPhonePortrait:  return 1080.0f / 1920.0f;
    case EditorContext::GameViewportAspect::iPhoneLandscape: return 1920.0f / 1080.0f;
    case EditorContext::GameViewportAspect::Free:
    default:                                            return 0.0f;
    }
}

const char* GetGameViewportAspectLabel(EditorContext::GameViewportAspect aspect)
{
    switch (aspect) {
    case EditorContext::GameViewportAspect::Ratio16x9:  return "16:9";
    case EditorContext::GameViewportAspect::Ratio4x3:   return "4:3";
    case EditorContext::GameViewportAspect::Ratio1x1:   return "1:1";
    case EditorContext::GameViewportAspect::Ratio9x16:  return "9:16";
    case EditorContext::GameViewportAspect::HD:          return "HD (1280x720)";
    case EditorContext::GameViewportAspect::FullHD:      return "Full HD (1920x1080)";
    case EditorContext::GameViewportAspect::QHD:         return "QHD (2560x1440)";
    case EditorContext::GameViewportAspect::UHD4K:       return "4K UHD (3840x2160)";
    case EditorContext::GameViewportAspect::WXGA:        return "WXGA (1280x800)";
    case EditorContext::GameViewportAspect::WUXGA:       return "WUXGA (1920x1200)";
    case EditorContext::GameViewportAspect::iPhonePortrait:  return "iPhone Portrait";
    case EditorContext::GameViewportAspect::iPhoneLandscape: return "iPhone Landscape";
    case EditorContext::GameViewportAspect::Free:
    default:                                            return "Free";
    }
}

void DrawGameViewportAspectControl(EditorContext& ctx)
{
    if (ImGui::BeginCombo("##game_aspect", GetGameViewportAspectLabel(ctx.gameViewportAspect))) {
        constexpr EditorContext::GameViewportAspect kAspects[] = {
            EditorContext::GameViewportAspect::Free,
            EditorContext::GameViewportAspect::Ratio16x9,
            EditorContext::GameViewportAspect::Ratio4x3,
            EditorContext::GameViewportAspect::Ratio1x1,
            EditorContext::GameViewportAspect::Ratio9x16,
            EditorContext::GameViewportAspect::HD,
            EditorContext::GameViewportAspect::FullHD,
            EditorContext::GameViewportAspect::QHD,
            EditorContext::GameViewportAspect::UHD4K,
            EditorContext::GameViewportAspect::WXGA,
            EditorContext::GameViewportAspect::WUXGA,
            EditorContext::GameViewportAspect::iPhonePortrait,
            EditorContext::GameViewportAspect::iPhoneLandscape
        };
        for (EditorContext::GameViewportAspect aspect : kAspects) {
            const bool selected = ctx.gameViewportAspect == aspect;
            if (ImGui::Selectable(GetGameViewportAspectLabel(aspect), selected))
                ctx.gameViewportAspect = aspect;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

ImVec2 FitSizeToAspect(ImVec2 size, float aspect)
{
    if (aspect <= 0.0f) return size;
    const float availableAspect = size.x / size.y;
    if (availableAspect > aspect)
        size.x = size.y * aspect;
    else
        size.y = size.x / aspect;
    return size;
}

} // namespace

ViewportPanel::ViewportPanel(Kind kind)
    : m_kind(kind)
    , m_windowName(kind == Kind::Scene ? "Scene" : (kind == Kind::Game ? "Game" : "UI"))
{
}

void ViewportPanel::OnBeforeBegin(EditorContext& ctx)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, 0.0f });
    if (m_kind == Kind::Game && ctx.requestGameViewportFocus)
        ImGui::SetNextWindowFocus();
}

void ViewportPanel::OnAfterBegin(EditorContext& ctx)
{
    if (m_kind == Kind::Game)
        ctx.requestGameViewportFocus = false;
    ImGui::PopStyleVar();
}

void ViewportPanel::OnRenderContent(EditorContext& ctx)
{
    const bool isSceneView = m_kind == Kind::Scene;
    const bool isGameView = m_kind == Kind::Game;
    const bool isUIView = m_kind == Kind::UI;
    if (isGameView) {
        ImGui::SetNextItemWidth(96.0f);
        DrawGameViewportAspectControl(ctx);
    }

    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 1.0f) size.x = 1.0f;
    if (size.y < 1.0f) size.y = 1.0f;

    if (isGameView || isUIView) {
        size = FitSizeToAspect(size, GetGameViewportAspectRatio(ctx.gameViewportAspect));
        if (size.x < 1.0f) size.x = 1.0f;
        if (size.y < 1.0f) size.y = 1.0f;
    }

    if (isSceneView) {
        ctx.viewportWidth = size.x;
        ctx.viewportHeight = size.y;
        ctx.viewportFocused = ImGui::IsWindowFocused();
    } else if (isGameView) {
        ctx.gameViewportWidth = size.x;
        ctx.gameViewportHeight = size.y;
        ctx.gameViewportFocused = ImGui::IsWindowFocused();
    } else if (isUIView) {
        ctx.uiViewportWidth = size.x;
        ctx.uiViewportHeight = size.y;
        ctx.uiViewportFocused = ImGui::IsWindowFocused();
    }

    ImVec2 viewportMin = ImGui::GetCursorScreenPos();
    if (isGameView || isUIView) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        viewportMin.x += (std::max)(0.0f, (available.x - size.x) * 0.5f);
        viewportMin.y += (std::max)(0.0f, (available.y - size.y) * 0.5f);
        if (isGameView) {
            ctx.gameViewportOriginX = viewportMin.x;
            ctx.gameViewportOriginY = viewportMin.y;
        } else {
            ctx.uiViewportOriginX = viewportMin.x;
            ctx.uiViewportOriginY = viewportMin.y;
        }
    }
    ImVec2 viewportMax = { viewportMin.x + size.x, viewportMin.y + size.y };
    bool viewportHovered = ImGui::IsMouseHoveringRect(viewportMin, viewportMax);
    if (hdrRT.IsValid() && renderer && resources) {
        ImTextureID texID = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(renderer->GetImTextureID(hdrRT, *resources, 0)));
        ImGui::GetWindowDrawList()->AddImage(texID, viewportMin, viewportMax);
    } else {
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            cursor, { cursor.x + size.x, cursor.y + size.y }, IM_COL32(30, 30, 30, 255));
        ImGui::SetCursorScreenPos({ cursor.x + size.x * 0.5f - 60.0f, cursor.y + size.y * 0.5f - 7.0f });
        ImGui::TextDisabled("No Render Target");
    }

    const bool inPlayOrPause = ctx.playMode && !ctx.playMode->IsInEditor();

    const bool gizmoWantsMouse = ImGuizmo::IsUsing() || ImGuizmo::IsOver()
                              || ImGuizmo::IsUsingViewManipulate() || ImGuizmo::IsViewManipulateHovered();
    if (isSceneView && !inPlayOrPause && viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !gizmoWantsMouse)
        PickEntity(ctx, viewportMin);

    if (isSceneView && !inPlayOrPause) {
        DrawGizmo(ctx, viewportMin, size);
        DrawOrientationGizmo(ctx, viewportMin, size);
    }

    // Play / Pause 中はボーダーで状態を示す
    if (isGameView && ctx.playMode && ctx.playMode->IsPlaying())
        ImGui::GetWindowDrawList()->AddRect(viewportMin, viewportMax, IM_COL32(80, 200, 80, 220), 0.0f, 0, 3.0f);
    else if (isGameView && ctx.playMode && ctx.playMode->IsPaused())
        ImGui::GetWindowDrawList()->AddRect(viewportMin, viewportMax, IM_COL32(255, 180, 50, 220), 0.0f, 0, 3.0f);

    if (isSceneView && ctx.showSceneStats) {
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
