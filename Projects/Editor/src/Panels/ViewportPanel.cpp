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
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>
#include <Math/Ray.hpp>
#include <imgui.h>
#include <ImGuizmo.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fbzz::editor {

namespace {

// ImGuizmo 縺ｯ OpenGL 蛻怜━蜈郁ｦ冗ｴ・ｒ蜑肴署縺ｨ縺吶ｋ縺後√お繝ｳ繧ｸ繝ｳ縺ｯ陦悟━蜈医〒陦悟・繧呈ｼ邏阪☆繧九・
// Transpose 縺吶ｋ縺薙→縺ｧ ImGuizmo 縺梧悄蠕・☆繧九Γ繝｢繝ｪ繝ｬ繧､繧｢繧ｦ繝・(蛻怜━蜈・ 縺ｫ螟画鋤縺吶ｋ縲・
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

    if (best.IsValid() && !ctx.IsLocked(best)) {
        if (!ImGui::GetIO().KeyCtrl) ctx.selectedEntities.clear();
        if (std::find(ctx.selectedEntities.begin(), ctx.selectedEntities.end(), best) == ctx.selectedEntities.end())
            ctx.selectedEntities.push_back(best);
        return true;
    }

    if (!ImGui::GetIO().KeyCtrl) ctx.selectedEntities.clear();
    return false;
}

// UI繝薙Η繝ｼ繝昴・繝医〒繧ｯ繝ｪ繝・け縺励◆ UI 隕∫ｴ繧帝∈謚槭☆繧・
void PickUIEntity(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize)
{
    if (!ctx.activeScene) return;

    float canvasW = 1920.0f, canvasH = 1080.0f;
    for (auto& other : ctx.activeScene->GameObjects()) {
        if (auto* canvas = other.GetComponent<scene::UICanvas>()) {
            canvasW = canvas->canvasWidth;
            canvasH = canvas->canvasHeight;
            break;
        }
    }

    const ImVec2 mouse = ImGui::GetMousePos();
    const float cx = (mouse.x - viewportMin.x) / viewportSize.x * canvasW;
    const float cy = (mouse.y - viewportMin.y) / viewportSize.y * canvasH;

    scene::EntityID best = {};
    float bestArea = FLT_MAX;

    for (auto& go : ctx.activeScene->GameObjects()) {
        auto* img = go.GetComponent<scene::UIImage>();
        auto* txt = go.GetComponent<scene::UIText>();
        if (!img && !txt) continue;

        const float ox = go.transform.localPosition.x;
        const float oy = go.transform.localPosition.y;

        if (img) {
            const float ow = go.transform.localScale.x;
            const float oh = go.transform.localScale.y;
            if (cx >= ox && cx <= ox + ow && cy >= oy && cy <= oy + oh) {
                const float area = ow * oh;
                if (area < bestArea) { bestArea = area; best = go.GetID(); }
            }
        } else {
            constexpr float kHitR = 20.0f;
            if (std::abs(cx - ox) < kHitR && std::abs(cy - oy) < kHitR) {
                if (0.0f < bestArea) { bestArea = 0.0f; best = go.GetID(); }
            }
        }
    }

    if (!ImGui::GetIO().KeyCtrl) ctx.selectedEntities.clear();
    if (best.IsValid()) ctx.selectedEntities.push_back(best);
}

// --- UI 繧ｮ繧ｺ繝｢逕ｨ繝倥Ν繝代・ ---

// 濶ｲ螳壽焚
constexpr ImU32 kUIColX       = IM_COL32(220,  60,  60, 255);
constexpr ImU32 kUIColXHov    = IM_COL32(255, 160, 160, 255);
constexpr ImU32 kUIColY       = IM_COL32( 60, 200,  60, 255);
constexpr ImU32 kUIColYHov    = IM_COL32(160, 255, 160, 255);
constexpr ImU32 kUIColCtr     = IM_COL32(220, 200,  60, 255);
constexpr ImU32 kUIColCtrHov  = IM_COL32(255, 240, 140, 255);
constexpr ImU32 kUIColHandle  = IM_COL32(  0, 200, 255, 220);
constexpr ImU32 kUIColHndHov  = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kUIColRing    = IM_COL32(  0, 200, 255, 200);
constexpr ImU32 kUIColOutline = IM_COL32(  0, 180, 255, 140);
constexpr float kUIArrowLen   = 55.0f;  // screen px
constexpr float kUIHandleR    = 5.0f;
constexpr float kUICenterR    = 6.0f;

void DrawArrow2D(ImDrawList* dl, ImVec2 from, ImVec2 to, ImU32 col, float thickness = 2.5f)
{
    constexpr float kHead = 11.0f;
    dl->AddLine(from, to, col, thickness);
    const float dx = to.x - from.x, dy = to.y - from.y;
    const float len = std::sqrt(dx*dx + dy*dy);
    if (len < 0.01f) return;
    const float nx = dx/len, ny = dy/len;
    const float px = -ny * kHead * 0.45f, py = nx * kHead * 0.45f;
    dl->AddTriangleFilled(to,
        { to.x - nx*kHead + px, to.y - ny*kHead + py },
        { to.x - nx*kHead - px, to.y - ny*kHead - py },
        col);
}

bool IsMouseNearLine(ImVec2 a, ImVec2 b, float tol = 7.0f)
{
    const ImVec2 m = ImGui::GetMousePos();
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx*dx + dy*dy;
    if (len2 < 0.01f) return false;
    float t = ((m.x - a.x)*dx + (m.y - a.y)*dy) / len2;
    t = std::clamp(t, 0.0f, 1.0f);
    const float cx = a.x + t*dx, cy = a.y + t*dy;
    return (m.x-cx)*(m.x-cx) + (m.y-cy)*(m.y-cy) < tol*tol;
}

// UI繝薙Η繝ｼ繝昴・繝亥ｰら畑 2D 繧ｮ繧ｺ繝｢ (QWER 縺ｧ 3D 繧ｮ繧ｺ繝｢縺ｨ蜷後§謫堺ｽ懈─)
//   W = 遘ｻ蜍・(X/Y 霆ｸ蛻ｶ邏・+ 閾ｪ逕ｱ遘ｻ蜍・
//   E = 蝗櫁ｻ｢ (Z霆ｸ, 繝ｪ繝ｳ繧ｰ謫堺ｽ・
//   R = 繝ｪ繧ｵ繧､繧ｺ (8繝上Φ繝峨Ν, 荳ｭ蠢・崋螳・
//   Q = World/Local 繝医げ繝ｫ (3D 繧ｮ繧ｺ繝｢縺ｨ蜈ｱ譛・
// 謌ｻ繧雁､: true = 繝槭え繧ｹ繧ｯ繝ｪ繝・け繧偵ぐ繧ｺ繝｢縺梧ｶ郁ｲｻ貂医∩
bool DrawUIGizmo(EditorContext& ctx,
                 const ImVec2& viewportMin,
                 const ImVec2& viewportSize,
                 int& drag,
                 ImVec2& dragStart,
                 float& startX,
                 float& startY,
                 float& startWidth,
                 float& startHeight,
                 float& startAngle,
                 float& startZ)
{
    HandleGizmoShortcuts(ctx);

    scene::GameObject* go = ctx.GetSelectedGO();
    if (!go || !ctx.activeScene) return false;

    auto* img = go->GetComponent<scene::UIImage>();
    auto* txt = go->GetComponent<scene::UIText>();
    if (!img && !txt) return false;

    float canvasW = 1920.0f, canvasH = 1080.0f;
    for (auto& other : ctx.activeScene->GameObjects()) {
        if (auto* canvas = other.GetComponent<scene::UICanvas>()) {
            canvasW = canvas->canvasWidth;
            canvasH = canvas->canvasHeight;
            break;
        }
    }

    const float scaleX = viewportSize.x / canvasW;
    const float scaleY = viewportSize.y / canvasH;
    auto toScreen = [&](float cx, float cy) -> ImVec2 {
        return { viewportMin.x + cx * scaleX, viewportMin.y + cy * scaleY };
    };

    auto& t = go->transform;
    const float px = t.localPosition.x, py = t.localPosition.y;
    const float sw = img ? t.localScale.x : 0.0f;
    const float sh = img ? t.localScale.y : 0.0f;

    // Z 蝗櫁ｻ｢繧・quaternion 縺九ｉ蜿悶ｊ蜃ｺ縺・
    const math::Quaternion& q = t.localRotation;
    const float zAngle = std::atan2f(2.0f*(q.w*q.z + q.x*q.y),
                                      1.0f - 2.0f*(q.y*q.y + q.z*q.z));
    const float cosZ = std::cosf(zAngle), sinZ = std::sinf(zAngle);

    // 遏ｩ蠖｢荳ｭ蠢・(繧ｭ繝｣繝ｳ繝舌せ/繧ｹ繧ｯ繝ｪ繝ｼ繝ｳ)
    const float cenCX = px + sw * 0.5f, cenCY = py + sh * 0.5f;
    const ImVec2 centerScr = toScreen(cenCX, cenCY);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool wantsMouse = false;

    // drag: 繝峨Λ繝・げ謫堺ｽ應ｸｭ縺ｮ繝｢繝ｼ繝峨ｒ菫晄戟縺吶ｋ繧ｹ繝・・繝医・繧ｷ繝ｳ螟画焚縲・
    //   -1  = 謫堺ｽ懊↑縺・
    //    0  = 閾ｪ逕ｱ遘ｻ蜍・(荳ｭ蠢・け繝ｪ繝・け)
    //    1  = X 霆ｸ諡俶據遘ｻ蜍・
    //    2  = Y 霆ｸ諡俶據遘ｻ蜍・
    //  3-10 = 繝ｪ繧ｵ繧､繧ｺ繝上Φ繝峨Ν 0-7 (蟾ｦ荳岩・蜿ｳ荳・ 譎りｨ亥屓繧・
    //   20  = 蝗櫁ｻ｢繝ｪ繝ｳ繧ｰ
    // IsMouseReleased 縺ｧ繝ｪ繧ｻ繝・ヨ縺輔ｌ繧九◆繧√ヵ繝ｬ繝ｼ繝繧偵∪縺溘＞縺ｧ菫晄戟縺輔ｌ繧九・
    const EditorContext::GizmoMode mode = ctx.gizmoMode;

    // --- 蝗櫁ｻ｢繧定・・縺励◆遏ｩ蠖｢繧｢繧ｦ繝医Λ繧､繝ｳ (蟶ｸ譎り｡ｨ遉ｺ) ---
    auto rotOfs = [&](float lx, float ly) -> ImVec2 {
        return {
            centerScr.x + (lx*cosZ - ly*sinZ) * scaleX,
            centerScr.y + (lx*sinZ + ly*cosZ) * scaleY
        };
    };
    if (img) {
        const float hW = sw * 0.5f, hH = sh * 0.5f;
        ImVec2 c[4] = { rotOfs(-hW,-hH), rotOfs(hW,-hH), rotOfs(hW,hH), rotOfs(-hW,hH) };
        dl->AddQuad(c[0], c[1], c[2], c[3], kUIColOutline, 1.5f);
    }

    // --- 繧ｹ繧ｯ繝ｪ繝ｼ繝ｳ遨ｺ髢薙・繝ｭ繝ｼ繧ｫ繝ｫ霆ｸ繝吶け繝医Ν (蜊倅ｽ埼聞) ---
    auto screenUnitAxis = [&](float ax, float ay) -> ImVec2 {
        ImVec2 v = { ax * scaleX, ay * scaleY };
        const float len = std::sqrt(v.x*v.x + v.y*v.y);
        return (len > 0.001f) ? ImVec2{ v.x/len, v.y/len } : ImVec2{ 1, 0 };
    };
    const ImVec2 dirX = screenUnitAxis( cosZ,  sinZ);
    const ImVec2 dirY = screenUnitAxis( sinZ, -cosZ);
    const ImVec2 tipX = { centerScr.x + dirX.x * kUIArrowLen, centerScr.y + dirX.y * kUIArrowLen };
    const ImVec2 tipY = { centerScr.x + dirY.x * kUIArrowLen, centerScr.y + dirY.y * kUIArrowLen };

    // ==============================
    // W : 遘ｻ蜍輔Δ繝ｼ繝・
    // ==============================
    if (mode == EditorContext::GizmoMode::Translate) {
        const bool hovX = IsMouseNearLine(centerScr, tipX, 7.0f);
        const ImVec2 mp = ImGui::GetMousePos();
        const float distC = std::sqrt((mp.x-centerScr.x)*(mp.x-centerScr.x)+(mp.y-centerScr.y)*(mp.y-centerScr.y));
        const bool hovY = !hovX && IsMouseNearLine(centerScr, tipY, 7.0f);
        const bool hovC = !hovX && !hovY && distC < kUICenterR + 4.0f;

        if (hovX || hovY || hovC) wantsMouse = true;

        DrawArrow2D(dl, centerScr, tipX, hovX ? kUIColXHov : kUIColX);
        DrawArrow2D(dl, centerScr, tipY, hovY ? kUIColYHov : kUIColY);
        dl->AddCircleFilled(centerScr, kUICenterR, hovC ? kUIColCtrHov : kUIColCtr);
        dl->AddCircle(centerScr, kUICenterR + 1.0f, IM_COL32(0,0,0,120));

        // UIText 縺ｮ蝣ｴ蜷医・螟門捉縺ｮ蜊∝ｭ励ｂ謠上￥
        if (txt) {
            constexpr float kR = 10.0f;
            dl->AddLine({ centerScr.x - kR, centerScr.y }, { centerScr.x + kR, centerScr.y }, IM_COL32(0,200,255,180), 1.5f);
            dl->AddLine({ centerScr.x, centerScr.y - kR }, { centerScr.x, centerScr.y + kR }, IM_COL32(0,200,255,180), 1.5f);
        }

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if      (hovX) { drag = 1; }
            else if (hovY) { drag = 2; }
            else if (hovC) { drag = 0; }
            if (drag >= 0) { dragStart = mp; startX = px; startY = py; }
        }

        if (drag >= 0 && drag <= 2 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 mm = ImGui::GetMousePos();
            const float dcx = (mm.x - dragStart.x) / scaleX;
            const float dcy = (mm.y - dragStart.y) / scaleY;
            float newX = startX, newY = startY;
            if (drag == 0) {
                newX = startX + dcx;
                newY = startY + dcy;
            } else if (drag == 1) {
                const float proj = dcx * cosZ + dcy * sinZ;
                newX = startX + proj * cosZ;
                newY = startY + proj * sinZ;
            } else {
                const float proj = dcx * (-sinZ) + dcy * cosZ;
                newX = startX + proj * (-sinZ);
                newY = startY + proj * cosZ;
            }
            if (ctx.snapEnabled) { newX = std::round(newX); newY = std::round(newY); }
            t.localPosition.x = newX;
            t.localPosition.y = newY;
        }
    }
    // ==============================
    // E : 蝗櫁ｻ｢繝｢繝ｼ繝・
    // ==============================
    else if (mode == EditorContext::GizmoMode::Rotate) {
        const float hW = sw * 0.5f * scaleX, hH = sh * 0.5f * scaleY;
        const float ringR = std::sqrt(hW*hW + hH*hH) + 22.0f;

        dl->AddCircle(centerScr, ringR, kUIColRing, 64, 2.0f);

        const ImVec2 mp = ImGui::GetMousePos();
        const float dist = std::sqrt((mp.x-centerScr.x)*(mp.x-centerScr.x)+(mp.y-centerScr.y)*(mp.y-centerScr.y));
        const bool hovRing = std::abs(dist - ringR) < 8.0f;
        if (hovRing) {
            dl->AddCircle(centerScr, ringR, IM_COL32(255,255,255,180), 64, 2.5f);
            wantsMouse = true;
        }

        // 隗貞ｺｦ繧､繝ｳ繧ｸ繧ｱ繝ｼ繧ｿ繝ｼ
        const ImVec2 rotTip = { centerScr.x + cosZ * ringR, centerScr.y + sinZ * ringR };
        dl->AddLine(centerScr, rotTip, IM_COL32(0,200,255,180), 1.5f);
        dl->AddCircleFilled(rotTip, 4.0f, IM_COL32(0,200,255,255));

        if (hovRing && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            drag = 20;
            startAngle = std::atan2f(mp.y - centerScr.y, mp.x - centerScr.x);
            startZ = zAngle;
        }

        if (drag == 20 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 mm = ImGui::GetMousePos();
            const float curAngle = std::atan2f(mm.y - centerScr.y, mm.x - centerScr.x);
            float newZ = startZ + (curAngle - startAngle);
            if (ctx.snapEnabled) {
                constexpr float k15deg = 3.14159265f / 12.0f;
                newZ = std::round(newZ / k15deg) * k15deg;
            }
            t.localRotation = math::Quaternion::FromEuler({ 0.0f, 0.0f, newZ });
        }

        // 隗貞ｺｦ繝・く繧ｹ繝・
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f deg", zAngle * (180.0f / 3.14159265f));
        dl->AddText({ centerScr.x + ringR + 6.0f, centerScr.y - 7.0f },
                    IM_COL32(200, 220, 255, 200), buf);
    }
    // ==============================
    // R : 繧ｹ繧ｱ繝ｼ繝ｫ繝｢繝ｼ繝・(UIImage 縺ｮ縺ｿ)
    // ==============================
    else if (mode == EditorContext::GizmoMode::Scale && img) {
        const float hW = sw * 0.5f, hH = sh * 0.5f;
        const ImVec2 handles[8] = {
            rotOfs(-hW,-hH), rotOfs(0,-hH), rotOfs(hW,-hH),
            rotOfs(-hW,  0),                rotOfs(hW,  0),
            rotOfs(-hW, hH), rotOfs(0, hH), rotOfs(hW, hH),
        };

        for (int i = 0; i < 8; ++i) {
            const ImVec2 hMin = { handles[i].x - kUIHandleR, handles[i].y - kUIHandleR };
            const ImVec2 hMax = { handles[i].x + kUIHandleR, handles[i].y + kUIHandleR };
            const bool hov = ImGui::IsMouseHoveringRect(hMin, hMax);
            if (hov) wantsMouse = true;
            dl->AddRectFilled(hMin, hMax, hov ? kUIColHndHov : kUIColHandle);
            dl->AddRect(hMin, hMax, IM_COL32(0,0,0,100));
            if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                drag = 3 + i; dragStart = ImGui::GetMousePos();
                startX = px; startY = py; startWidth = sw; startHeight = sh;
            }
        }

        if (drag >= 3 && drag <= 10 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 mm = ImGui::GetMousePos();
            const float dcx = (mm.x - dragStart.x) / scaleX;
            const float dcy = (mm.y - dragStart.y) / scaleY;
            // 繝ｭ繝ｼ繧ｫ繝ｫ霆ｸ縺ｫ繝励Ο繧ｸ繧ｧ繧ｯ繧ｷ繝ｧ繝ｳ
            const float projX =  dcx * cosZ + dcy * sinZ;
            const float projY = -dcx * sinZ + dcy * cosZ;

            const int hi = drag - 3;
            float nw = startWidth, nh = startHeight;
            if (hi == 0 || hi == 3 || hi == 5) nw = startWidth - projX; // left
            if (hi == 2 || hi == 4 || hi == 7) nw = startWidth + projX; // right
            if (hi == 0 || hi == 1 || hi == 2) nh = startHeight - projY; // top
            if (hi == 5 || hi == 6 || hi == 7) nh = startHeight + projY; // bottom
            nw = (std::max)(1.0f, nw);
            nh = (std::max)(1.0f, nh);
            if (ctx.snapEnabled) { nw = std::round(nw); nh = std::round(nh); }

            // 荳ｭ蠢・ｒ蝗ｺ螳壹＠縺ｦ菴咲ｽｮ繧定ｪｿ謨ｴ
            t.localScale.x = nw;
            t.localScale.y = nh;
            t.localPosition.x = (startX + startWidth * 0.5f) - nw * 0.5f;
            t.localPosition.y = (startY + startHeight * 0.5f) - nh * 0.5f;
        }
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        drag = -1;

    return wantsMouse || drag != -1;
}

// 繝ｯ繝ｼ繝ｫ繝牙ｺｧ讓・竊・繝薙Η繝ｼ繝昴・繝医せ繧ｯ繝ｪ繝ｼ繝ｳ蠎ｧ讓吶∈謚募ｽｱ (w<=0 縺ｪ繧・false)
bool WorldToScreen(const math::Vector3& world,
                   const EditorContext& ctx,
                   const ImVec2& vpMin, const ImVec2& vpSize,
                   ImVec2& out)
{
    if (!ctx.editorCamera) return false;
    const math::Matrix4 vp = ctx.editorCamera->GetProjectionMatrix()
                           * ctx.editorCamera->GetViewMatrix();
    const math::Vector4 clip = vp * math::Vector4{ world.x, world.y, world.z, 1.0f };
    if (clip.w <= 0.001f) return false;
    const float ndcX =  clip.x / clip.w;
    const float ndcY = -clip.y / clip.w;
    out = {
        vpMin.x + (ndcX * 0.5f + 0.5f) * vpSize.x,
        vpMin.y + (ndcY * 0.5f + 0.5f) * vpSize.y
    };
    return true;
}

void DrawSelectionOutline(EditorContext& ctx, const ImVec2& vpMin, const ImVec2& vpSize)
{
    if (!ctx.activeScene || !ctx.editorCamera || ctx.selectedEntities.empty()) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr ImU32 kOuterCol  = IM_COL32(255, 165,  0,  55);
    constexpr ImU32 kInnerCol  = IM_COL32(255, 185, 50, 230);
    constexpr float kThickOuter = 4.0f;
    constexpr float kThickInner = 1.5f;

    auto transformByWorld = [](const math::Matrix4& m, const math::Vector3& p) -> math::Vector3 {
        math::Vector4 v = m * math::Vector4{ p.x, p.y, p.z, 1.0f };
        if (v.w > 0.0001f) v = v * (1.0f / v.w);
        return { v.x, v.y, v.z };
    };

    for (scene::EntityID id : ctx.selectedEntities) {
        scene::GameObject* go = ctx.activeScene->GetGameObject(id);
        if (!go) continue;

        const math::Matrix4 world = go->transform.GetWorldMatrix();
        auto* mr = go->GetComponent<scene::MeshRenderer>();

        // 繝｡繝・す繝･縺九ｉ AABB 繧定ｨ育ｮ・
        math::Vector3 bMin = {}, bMax = {};
        bool hasBounds = false;
        if (mr && mr->mesh && !mr->mesh->cpuVertices.empty()) {
            bMin = bMax = mr->mesh->cpuVertices[0].position;
            for (const auto& v : mr->mesh->cpuVertices) {
                bMin.x = (std::min)(bMin.x, v.position.x);
                bMin.y = (std::min)(bMin.y, v.position.y);
                bMin.z = (std::min)(bMin.z, v.position.z);
                bMax.x = (std::max)(bMax.x, v.position.x);
                bMax.y = (std::max)(bMax.y, v.position.y);
                bMax.z = (std::max)(bMax.z, v.position.z);
            }
            hasBounds = true;
        }

        if (!hasBounds) {
            // 繝｡繝・す繝･縺ｪ縺・ 繝斐・繝・ヨ轤ｹ縺ｫ蜀・ｒ謠上￥
            ImVec2 sp;
            if (WorldToScreen(go->transform.position, ctx, vpMin, vpSize, sp)) {
                dl->AddCircle(sp, 16.0f, kOuterCol,  24, kThickOuter);
                dl->AddCircle(sp, 16.0f, kInnerCol,  24, kThickInner);
            }
            continue;
        }

        // AABB 縺ｮ 8 繧ｳ繝ｼ繝翫・繧偵Ρ繝ｼ繝ｫ繝臥ｩｺ髢薙∈螟画鋤縺励※繧ｹ繧ｯ繝ｪ繝ｼ繝ｳ謚募ｽｱ
        const math::Vector3 localCorners[8] = {
            {bMin.x, bMin.y, bMin.z}, {bMax.x, bMin.y, bMin.z},
            {bMax.x, bMax.y, bMin.z}, {bMin.x, bMax.y, bMin.z},
            {bMin.x, bMin.y, bMax.z}, {bMax.x, bMin.y, bMax.z},
            {bMax.x, bMax.y, bMax.z}, {bMin.x, bMax.y, bMax.z},
        };

        ImVec2 sc[8];
        bool   vis[8];
        for (int i = 0; i < 8; ++i)
            vis[i] = WorldToScreen(transformByWorld(world, localCorners[i]), ctx, vpMin, vpSize, sc[i]);

        // AABB 縺ｮ 12 霎ｺ
        constexpr int kEdges[12][2] = {
            {0,1},{1,2},{2,3},{3,0},   // 蜑埼擇 4 霎ｺ
            {4,5},{5,6},{6,7},{7,4},   // 閭碁擇 4 霎ｺ
            {0,4},{1,5},{2,6},{3,7},   // 謗･邯・4 霎ｺ
        };
        for (const auto& e : kEdges) {
            if (!vis[e[0]] || !vis[e[1]]) continue;
            dl->AddLine(sc[e[0]], sc[e[1]], kOuterCol,  kThickOuter);
            dl->AddLine(sc[e[0]], sc[e[1]], kInnerCol,  kThickInner);
        }
    }
}

void DrawSceneIcons(EditorContext& ctx, const ImVec2& vpMin, const ImVec2& vpSize)
{
    if (!ctx.activeScene || !ctx.editorCamera) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 vpMax = { vpMin.x + vpSize.x, vpMin.y + vpSize.y };

    for (auto& go : ctx.activeScene->GameObjects()) {
        if (!go.activeSelf()) continue;

        ImVec2 sp;
        if (!WorldToScreen(go.transform.position, ctx, vpMin, vpSize, sp)) continue;
        if (sp.x < vpMin.x || sp.x > vpMax.x || sp.y < vpMin.y || sp.y > vpMax.y) continue;

        const bool isSelected = std::find(ctx.selectedEntities.begin(),
                                          ctx.selectedEntities.end(),
                                          go.GetID()) != ctx.selectedEntities.end();
        const ImU32 selCol = IM_COL32(255, 220, 60, 255);

        if (auto* light = go.GetComponent<scene::LightComponent>()) {
            // 繝ｩ繧､繝医い繧､繧ｳ繝ｳ: 荳ｭ蠢・・ + 謾ｾ蟆・ｷ・
            constexpr float kR = 8.0f;
            constexpr float kRay = 14.0f;
            constexpr int   kRays = 8;
            const ImU32 col = isSelected ? selCol : IM_COL32(255, 200, 60, 200);
            dl->AddCircleFilled(sp, kR, col);
            dl->AddCircle(sp, kR, IM_COL32(0, 0, 0, 120), 16, 1.5f);
            for (int i = 0; i < kRays; ++i) {
                const float ang = static_cast<float>(i) * (2.0f * 3.14159265f / kRays);
                const ImVec2 a = { sp.x + std::cosf(ang) * (kR + 3.0f),
                                   sp.y + std::sinf(ang) * (kR + 3.0f) };
                const ImVec2 b = { sp.x + std::cosf(ang) * (kR + kRay),
                                   sp.y + std::sinf(ang) * (kR + kRay) };
                dl->AddLine(a, b, col, 1.5f);
            }
        } else if (go.GetComponent<scene::CameraComponent>()) {
            // 繧ｫ繝｡繝ｩ繧｢繧､繧ｳ繝ｳ: 遏ｩ蠖｢繝懊ョ繧｣ + 蜿ｰ蠖｢繝ｬ繝ｳ繧ｺ
            constexpr float kW = 16.0f, kH = 11.0f;
            constexpr float kLW = 7.0f, kLH = 5.0f, kLX = 9.0f;
            const ImU32 col  = isSelected ? selCol : IM_COL32(120, 200, 255, 200);
            const ImU32 dark = IM_COL32(0, 0, 0, 140);
            // 繝懊ョ繧｣
            dl->AddRectFilled({ sp.x - kW, sp.y - kH * 0.5f },
                              { sp.x + kW * 0.4f, sp.y + kH * 0.5f }, col, 2.0f);
            dl->AddRect({ sp.x - kW, sp.y - kH * 0.5f },
                        { sp.x + kW * 0.4f, sp.y + kH * 0.5f }, dark, 2.0f);
            // 繝ｬ繝ｳ繧ｺ蜿ｰ蠖｢
            ImVec2 lens[4] = {
                { sp.x + kW * 0.4f, sp.y - kLH },
                { sp.x + kLX,       sp.y - kLW  },
                { sp.x + kLX,       sp.y + kLW  },
                { sp.x + kW * 0.4f, sp.y + kLH  },
            };
            dl->AddConvexPolyFilled(lens, 4, col);
            dl->AddPolyline(lens, 4, dark, ImDrawFlags_Closed, 1.0f);
        }
    }
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

    // column-major 竊・row-major 縺ｫ謌ｻ縺・
    const math::Matrix4 viewRow = math::Matrix4::Transpose(viewCol);

    // 繧ｫ繝｡繝ｩ菴咲ｽｮ: pos = -R^T * t  (view 縺ｮ 3x3 = R^T, t = viewRow 縺ｮ蟷ｳ陦檎ｧｻ蜍募・)
    const float tx = viewRow.m[0][3], ty = viewRow.m[1][3], tz = viewRow.m[2][3];
    ctx.editorCamera->m_position = {
        -(viewRow.m[0][0]*tx + viewRow.m[1][0]*ty + viewRow.m[2][0]*tz),
        -(viewRow.m[0][1]*tx + viewRow.m[1][1]*ty + viewRow.m[2][1]*tz),
        -(viewRow.m[0][2]*tx + viewRow.m[1][2]*ty + viewRow.m[2][2]*tz)
    };

    // 繧ｫ繝｡繝ｩ蝗櫁ｻ｢: 繝ｯ繝ｼ繝ｫ繝牙屓霆｢陦悟・縺ｮ蛻・= view 縺ｮ 3x3 縺ｮ陦・竊・霆｢鄂ｮ縺励※ FromMatrix4
    math::Matrix4 rotMat = math::Matrix4::Identity();
    rotMat.m[0][0] = viewRow.m[0][0]; rotMat.m[0][1] = viewRow.m[1][0]; rotMat.m[0][2] = viewRow.m[2][0];
    rotMat.m[1][0] = viewRow.m[0][1]; rotMat.m[1][1] = viewRow.m[1][1]; rotMat.m[1][2] = viewRow.m[2][1];
    rotMat.m[2][0] = viewRow.m[0][2]; rotMat.m[2][1] = viewRow.m[1][2]; rotMat.m[2][2] = viewRow.m[2][2];
    ctx.editorCamera->m_rotation = math::Quaternion::FromMatrix4(rotMat);
}

void DrawGizmo(EditorContext& ctx,
               const ImVec2& viewportMin,
               const ImVec2& viewportSize,
               int& lastOp,
               int& lastMode,
               bool& prevOver,
               bool& prevUsing)
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
    const int opInt = static_cast<int>(op);
    const int modeInt = static_cast<int>(mode);
    if (lastOp != opInt || lastMode != modeInt) {
        lastOp = opInt;
        lastMode = modeInt;
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
    if (gizmoOver != prevOver || gizmoUsing != prevUsing) {
        prevOver = gizmoOver;
        prevUsing = gizmoUsing;
    }

    if (!gizmoUsing) return;

    math::Matrix4 worldRow = math::Matrix4::Transpose(worldCol);
    math::Matrix4 localRow = worldRow;
    if (scene::GameObject* parent = go->GetParent()) {
        const math::Matrix4 parentInv = math::Matrix4::Inverse(parent->transform.GetWorldMatrix());
        localRow = parentInv * worldRow;
    }

    // ImGuizmo 縺ｮ DecomposeMatrixToComponents 縺ｯ蟾ｦ謇狗ｳｻ繧ｪ繧､繝ｩ繝ｼ隗偵〒 TRS 繧定ｿ斐☆縺溘ａ縲・
    // 繧ｨ繝ｳ繧ｸ繝ｳ縺ｮ蜿ｳ謇狗ｳｻ繧ｯ繧ｩ繝ｼ繧ｿ繝九が繝ｳ縺ｨ隨ｦ蜿ｷ縺悟粋繧上↑縺・り｡悟・縺九ｉ逶ｴ謗･ TRS 繧呈歓蜃ｺ縺吶ｋ縺薙→縺ｧ蝗樣∩縺吶ｋ縲・
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
        DrawSelectionOutline(ctx, viewportMin, size);
        DrawSceneIcons(ctx, viewportMin, size);
        DrawGizmo(ctx, viewportMin, size, m_lastGizmoOp, m_lastGizmoMode, m_prevGizmoOver, m_prevGizmoUsing);
        DrawOrientationGizmo(ctx, viewportMin, size);

        // F 繧ｭ繝ｼ: 驕ｸ謚槭が繝悶ず繧ｧ繧ｯ繝医∈繝輔か繝ｼ繧ｫ繧ｹ (繝薙Η繝ｼ繝昴・繝医↓繧ｭ繝ｼ繝懊・繝峨ヵ繧ｩ繝ｼ繧ｫ繧ｹ縺後≠繧句ｴ蜷医・縺ｿ)
        if (ctx.viewportFocused && input::Input::KeyDown(input::KeyCode::F)) {
            if (auto* go = ctx.GetSelectedGO()) {
                ctx.focusTargetPosition    = go->transform.position;
                ctx.requestFocusOnSelected = true;
            }
        }
    }
    bool uiGizmoActive = false;
    if (isUIView && !inPlayOrPause)
        uiGizmoActive = DrawUIGizmo(ctx, viewportMin, size, m_uiGizmoDrag, m_uiGizmoDragStart, m_uiGizmoStartX, m_uiGizmoStartY, m_uiGizmoStartWidth, m_uiGizmoStartHeight, m_uiGizmoStartAngle, m_uiGizmoStartZ);
    if (isUIView && !inPlayOrPause && viewportHovered
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !uiGizmoActive)
        PickUIEntity(ctx, viewportMin, size);

    // Play / Pause 荳ｭ縺ｯ繝懊・繝繝ｼ縺ｧ迥ｶ諷九ｒ遉ｺ縺・
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
