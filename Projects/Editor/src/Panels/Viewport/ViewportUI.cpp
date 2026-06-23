// FBZZ Engine
// ViewportUI.cpp | fbzz::editor
// UI Viewport の Canvas ガイド、2Dピッキング、UI Gizmo
#include "ViewportCommon.hpp"
#include <Editor/Util/UndoStack.hpp>

namespace fbzz::editor {

namespace {

bool UITransformEquals(const scene::Transform& lhs, const scene::Transform& rhs)
{
    return lhs.position.x == rhs.position.x &&
           lhs.position.y == rhs.position.y &&
           lhs.position.z == rhs.position.z &&
           lhs.rotation.x == rhs.rotation.x &&
           lhs.rotation.y == rhs.rotation.y &&
           lhs.rotation.z == rhs.rotation.z &&
           lhs.rotation.w == rhs.rotation.w &&
           lhs.scale.x == rhs.scale.x &&
           lhs.scale.y == rhs.scale.y &&
           lhs.scale.z == rhs.scale.z;
}

} // namespace

bool IsCanvasEditorCanvas(const scene::UICanvas& canvas)
{
    return canvas.enabled && canvas.renderMode != scene::UIRenderMode::WorldSpace;
}

bool IsUnderCanvas(scene::GameObject& go, scene::EntityID canvasID)
{
    if (!canvasID.IsValid()) return true;
    scene::GameObject* current = &go;
    while (current) {
        if (current->GetID() == canvasID)
            return true;
        current = current->GetParent();
    }
    return false;
}

struct UITransform2D {
    math::Vector2 position = math::Vector2::ZERO;
    float rotationZ = 0.0f;
};

float ExtractUIZRotation(const math::Quaternion& q)
{
    return std::atan2f(2.0f * (q.w * q.z + q.x * q.y),
                       1.0f - 2.0f * (q.y * q.y + q.z * q.z));
}

math::Vector2 RotateUIVector(const math::Vector2& v, float angle)
{
    const float c = std::cosf(angle);
    const float s = std::sinf(angle);
    return { v.x * c - v.y * s, v.x * s + v.y * c };
}

UITransform2D ComposeUITransform(const UITransform2D& parent, const scene::Transform& local)
{
    // WHY: UI の scale.xy は矩形サイズであり、親サイズを子の移動量へ掛けると
    //      Editor と Play の双方で子要素が親から大きく外れる。
    // WHAT: 親の位置・回転だけを UI 階層として合成し、サイズは各要素の scale.xy を使う。
    const math::Vector2 localPos = { local.position.x, local.position.y };
    UITransform2D result{};
    result.position = parent.position + RotateUIVector(localPos, parent.rotationZ);
    result.rotationZ = parent.rotationZ + ExtractUIZRotation(local.rotation);
    return result;
}

UITransform2D ResolveUITransform(scene::GameObject& go, bool includeSelf)
{
    std::vector<scene::GameObject*> chain;
    scene::GameObject* current = includeSelf ? &go : go.GetParent();
    while (current) {
        if (auto* canvas = current->GetComponent<scene::UICanvas>(); canvas && IsCanvasEditorCanvas(*canvas))
            break;
        chain.push_back(current);
        current = current->GetParent();
    }

    UITransform2D resolved{};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        resolved = ComposeUITransform(resolved, (*it)->transform);
    return resolved;
}

const scene::UICanvas* FindCanvasEditorCanvas(const EditorContext& ctx)
{
    if (!ctx.activeScene) return nullptr;

    // WHY: Canvas Editor は ScreenSpace UI を編集するビューとして扱う。
    //      activeUICanvas → 選択中 Canvas → 最初の ScreenSpace 系 Canvas の順に決めることで、
    //      複数 Canvas の編集対象がフレームごとに揺れない。
    if (ctx.activeUICanvas.IsValid()) {
        if (scene::GameObject* go = ctx.activeScene->GetGameObject(ctx.activeUICanvas)) {
            if (auto* canvas = go->GetComponent<scene::UICanvas>(); canvas && IsCanvasEditorCanvas(*canvas))
                return canvas;
        }
    }

    if (scene::GameObject* selected = ctx.GetSelectedGO()) {
        if (auto* canvas = selected->GetComponent<scene::UICanvas>();
            canvas && IsCanvasEditorCanvas(*canvas)) {
            return canvas;
        }
    }

    for (auto& go : ctx.activeScene->GameObjects()) {
        if (auto* canvas = go.GetComponent<scene::UICanvas>();
            canvas && IsCanvasEditorCanvas(*canvas)) {
            return canvas;
        }
    }
    return nullptr;
}

float ResolveCanvasEditorScale(const scene::UICanvas& canvas, float viewportWidth, float viewportHeight)
{
    // WHY: Editor の gizmo / pick と実描画で Canvas Scaler の解釈が分かれると、
    //      UI Viewport で合わせた位置が Play 開始時にずれて見える。
    // WHAT: UISystem::ResolveCanvasScale と同じ式で、Game RT 上の論理 Canvas 範囲を求める。
    if (canvas.scaleMode != scene::UICanvasScaleMode::ScaleWithScreenSize)
        return 1.0f;

    const float refW = (std::max)(1.0f, canvas.referenceWidth);
    const float refH = (std::max)(1.0f, canvas.referenceHeight);
    const float scaleW = (std::max)(1.0f, viewportWidth) / refW;
    const float scaleH = (std::max)(1.0f, viewportHeight) / refH;
    const float match = std::clamp(canvas.matchWidthOrHeight, 0.0f, 1.0f);
    return std::exp(std::log(scaleW) * (1.0f - match) + std::log(scaleH) * match);
}

void GetCanvasEditorSize(const EditorContext& ctx, float& canvasW, float& canvasH)
{
    canvasW = 1920.0f;
    canvasH = 1080.0f;
    if (const scene::UICanvas* canvas = FindCanvasEditorCanvas(ctx)) {
        if (canvas->scaleMode == scene::UICanvasScaleMode::ScaleWithScreenSize) {
            const float viewportW = ctx.gameViewportWidth > 1.0f ? ctx.gameViewportWidth : ctx.uiViewportWidth;
            const float viewportH = ctx.gameViewportHeight > 1.0f ? ctx.gameViewportHeight : ctx.uiViewportHeight;
            const float scale = ResolveCanvasEditorScale(*canvas, viewportW, viewportH);
            canvasW = (std::max)(1.0f, viewportW) / scale;
            canvasH = (std::max)(1.0f, viewportH) / scale;
            return;
        }

        canvasW = (std::max)(1.0f, canvas->canvasWidth);
        canvasH = (std::max)(1.0f, canvas->canvasHeight);
    }
}

void DrawCanvasEditorGuides(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize)
{
    float canvasW = 1920.0f, canvasH = 1080.0f;
    GetCanvasEditorSize(ctx, canvasW, canvasH);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 viewportMax = { viewportMin.x + viewportSize.x, viewportMin.y + viewportSize.y };
    const ImU32 border = IM_COL32(120, 180, 255, 180);
    const ImU32 guide  = IM_COL32(120, 180, 255, 70);

    // WHAT: Canvas 外周、中央線、一般的な Safe Area 目安を描く。
    // WHY: UI 実描画は RT 側、編集補助は ImGui 側に分離すると、Game 出力へガイドが混入しない。
    dl->AddRect(viewportMin, viewportMax, border, 0.0f, 0, 1.5f);
    dl->AddLine({ viewportMin.x + viewportSize.x * 0.5f, viewportMin.y },
                { viewportMin.x + viewportSize.x * 0.5f, viewportMax.y }, guide, 1.0f);
    dl->AddLine({ viewportMin.x, viewportMin.y + viewportSize.y * 0.5f },
                { viewportMax.x, viewportMin.y + viewportSize.y * 0.5f }, guide, 1.0f);

    const float scaleX = viewportSize.x / canvasW;
    const float scaleY = viewportSize.y / canvasH;
    const float safeX = canvasW * 0.05f * scaleX;
    const float safeY = canvasH * 0.05f * scaleY;
    dl->AddRect({ viewportMin.x + safeX, viewportMin.y + safeY },
                { viewportMax.x - safeX, viewportMax.y - safeY },
                IM_COL32(120, 255, 180, 90), 0.0f, 0, 1.0f);
}

// Pick the topmost UI element clicked in the UI viewport.
void PickUIEntity(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize)
{
    if (!ctx.activeScene) return;

    float canvasW = 1920.0f, canvasH = 1080.0f;
    GetCanvasEditorSize(ctx, canvasW, canvasH);
    const scene::EntityID activeCanvas = ctx.activeUICanvas;

    const ImVec2 mouse = ImGui::GetMousePos();
    const float cx = (mouse.x - viewportMin.x) / viewportSize.x * canvasW;
    const float cy = (mouse.y - viewportMin.y) / viewportSize.y * canvasH;

    scene::EntityID best = {};
    float bestArea = FLT_MAX;

    for (auto& go : ctx.activeScene->GameObjects()) {
        if (!IsUnderCanvas(go, activeCanvas))
            continue;

        auto* img = go.GetComponent<scene::UIImage>();
        auto* txt = go.GetComponent<scene::UIText>();
        auto* canvas = go.GetComponent<scene::UICanvas>();
        if (!img && !txt && !canvas) continue;

        const UITransform2D resolved = ResolveUITransform(go, true);
        const float ox = resolved.position.x;
        const float oy = resolved.position.y;

        if (canvas && IsCanvasEditorCanvas(*canvas)) {
            if (cx >= 0.0f && cx <= canvas->canvasWidth && cy >= 0.0f && cy <= canvas->canvasHeight) {
                const float area = canvas->canvasWidth * canvas->canvasHeight;
                if (area < bestArea) { bestArea = area; best = go.GetID(); }
            }
        } else if (img) {
            const float ow = go.transform.scale.x;
            const float oh = go.transform.scale.y;
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
    if (best.IsValid()) {
        ctx.selectedEntities.push_back(best);
        if (scene::GameObject* picked = ctx.activeScene->GetGameObject(best)) {
            scene::GameObject* current = picked;
            while (current) {
                if (auto* canvas = current->GetComponent<scene::UICanvas>(); canvas && IsCanvasEditorCanvas(*canvas)) {
                    ctx.activeUICanvas = current->GetID();
                    break;
                }
                current = current->GetParent();
            }
        }
    }
}

// --- UI gizmo helpers ---

// Colors.
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

// 2D gizmo for UI viewport. Uses the same Q/W/E/R controls as the 3D gizmo.
//   W = translate, with constrained X/Y axes and free center drag
//   E = rotate around Z using the ring
//   R = resize UIImage with 8 handles
//   Q = toggle World/Local space
// Returns true when the gizmo consumed mouse input.
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
    GetCanvasEditorSize(ctx, canvasW, canvasH);

    const float scaleX = viewportSize.x / canvasW;
    const float scaleY = viewportSize.y / canvasH;
    auto toScreen = [&](float cx, float cy) -> ImVec2 {
        return { viewportMin.x + cx * scaleX, viewportMin.y + cy * scaleY };
    };

    auto& t = go->transform;
    struct UIGizmoUndoTracker {
        std::string instanceId;
        scene::Transform before;
        bool active = false;
    };
    static UIGizmoUndoTracker undo;
    const int dragBefore = drag;
    const scene::Transform transformBeforeDraw = t;
    const UITransform2D parentResolved = ResolveUITransform(*go, false);
    const UITransform2D resolved = ComposeUITransform(parentResolved, t);
    const float px = resolved.position.x, py = resolved.position.y;
    const float localPx = t.position.x, localPy = t.position.y;
    const float sw = img ? t.scale.x : 0.0f;
    const float sh = img ? t.scale.y : 0.0f;

    // Extract Z rotation from the quaternion.
    const math::Quaternion& q = t.rotation;
    const float localZAngle = ExtractUIZRotation(q);
    const float zAngle = resolved.rotationZ;
    const float cosZ = std::cosf(zAngle), sinZ = std::sinf(zAngle);

    // Rectangle center in canvas and screen space.
    const float cenCX = px + sw * 0.5f, cenCY = py + sh * 0.5f;
    const ImVec2 centerScr = toScreen(cenCX, cenCY);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool wantsMouse = false;

    // drag stores the active UI gizmo operation across frames:
    //   -1  = none
    //    0  = free translate from center
    //    1  = constrained X-axis translate
    //    2  = constrained Y-axis translate
    //  3-10 = resize handles 0-7
    //   20  = rotation ring
    const EditorContext::GizmoMode mode = ctx.gizmoMode;

    // Rotated rectangle outline.
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

    // Local axes converted to unit vectors in screen space.
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
    // W: translate mode.
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

        // UIText has no image bounds, so draw a small cross at its origin.
        if (txt) {
            constexpr float kR = 10.0f;
            dl->AddLine({ centerScr.x - kR, centerScr.y }, { centerScr.x + kR, centerScr.y }, IM_COL32(0,200,255,180), 1.5f);
            dl->AddLine({ centerScr.x, centerScr.y - kR }, { centerScr.x, centerScr.y + kR }, IM_COL32(0,200,255,180), 1.5f);
        }

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if      (hovX) { drag = 1; }
            else if (hovY) { drag = 2; }
            else if (hovC) { drag = 0; }
            if (drag >= 0) { dragStart = mp; startX = localPx; startY = localPy; }
        }

        if (drag >= 0 && drag <= 2 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 mm = ImGui::GetMousePos();
            const math::Vector2 canvasDelta = {
                (mm.x - dragStart.x) / scaleX,
                (mm.y - dragStart.y) / scaleY
            };
            math::Vector2 canvasMove = canvasDelta;
            float newX = startX, newY = startY;
            if (drag == 1) {
                const math::Vector2 axis = { cosZ, sinZ };
                const float proj = canvasDelta.x * axis.x + canvasDelta.y * axis.y;
                canvasMove = { axis.x * proj, axis.y * proj };
            } else if (drag == 2) {
                const math::Vector2 axis = { -sinZ, cosZ };
                const float proj = canvasDelta.x * axis.x + canvasDelta.y * axis.y;
                canvasMove = { axis.x * proj, axis.y * proj };
            }
            const math::Vector2 localMove = RotateUIVector(canvasMove, -parentResolved.rotationZ);
            newX = startX + localMove.x;
            newY = startY + localMove.y;
            if (ctx.snapEnabled) { newX = std::round(newX); newY = std::round(newY); }
            t.position.x = newX;
            t.position.y = newY;
        }
    }
    // ==============================
    // E: rotate mode.
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

        // Angle indicator.
        const ImVec2 rotTip = { centerScr.x + cosZ * ringR, centerScr.y + sinZ * ringR };
        dl->AddLine(centerScr, rotTip, IM_COL32(0,200,255,180), 1.5f);
        dl->AddCircleFilled(rotTip, 4.0f, IM_COL32(0,200,255,255));

        if (hovRing && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            drag = 20;
            startAngle = std::atan2f(mp.y - centerScr.y, mp.x - centerScr.x);
            startZ = localZAngle;
        }

        if (drag == 20 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 mm = ImGui::GetMousePos();
            const float curAngle = std::atan2f(mm.y - centerScr.y, mm.x - centerScr.x);
            float newZ = startZ + (curAngle - startAngle);
            if (ctx.snapEnabled) {
                constexpr float k15deg = 3.14159265f / 12.0f;
                newZ = std::round(newZ / k15deg) * k15deg;
            }
            t.rotation = math::Quaternion::FromEuler({ 0.0f, 0.0f, newZ });
        }

        // Angle label.
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f deg", zAngle * (180.0f / 3.14159265f));
        dl->AddText({ centerScr.x + ringR + 6.0f, centerScr.y - 7.0f },
                    IM_COL32(200, 220, 255, 200), buf);
    }
    // ==============================
    // R: scale mode for UIImage.
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
                startX = localPx; startY = localPy; startWidth = sw; startHeight = sh;
            }
        }

        if (drag >= 3 && drag <= 10 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 mm = ImGui::GetMousePos();
            const float dcx = (mm.x - dragStart.x) / scaleX;
            const float dcy = (mm.y - dragStart.y) / scaleY;
            // Project mouse delta onto local axes.
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

            // Keep the center fixed while resizing from handles.
            t.scale.x = nw;
            t.scale.y = nh;
            t.position.x = (startX + startWidth * 0.5f) - nw * 0.5f;
            t.position.y = (startY + startHeight * 0.5f) - nh * 0.5f;
        }
    }

    if (!undo.active && dragBefore == -1 && drag != -1) {
        undo.instanceId = go->instanceId;
        undo.before = transformBeforeDraw;
        undo.active = true;
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (undo.active && ctx.activeScene) {
            const scene::Transform after = t;
            const scene::Transform before = undo.before;
            const std::string instanceId = undo.instanceId;
            scene::Scene* scene = ctx.activeScene;
            const std::function<void()> markDirty = ctx.markSceneDirty;
            auto apply = [scene, instanceId, markDirty](const scene::Transform& value) {
                if (auto* target = scene->FindByGuid(instanceId)) {
                    target->transform = value;
                    if (markDirty) markDirty();
                }
            };
            if (ctx.undoStack && !UITransformEquals(before, after)) {
                ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                    "Edit UI Transform",
                    [apply, after]() { apply(after); },
                    [apply, before]() { apply(before); }));
            } else if (!UITransformEquals(before, after) && markDirty) {
                markDirty();
            }
        }
        undo.active = false;
        drag = -1;
    }

    return wantsMouse || drag != -1;
}


} // namespace fbzz::editor
