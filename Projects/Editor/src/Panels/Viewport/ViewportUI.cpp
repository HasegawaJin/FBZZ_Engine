/// @file    ViewportUI.cpp
/// @brief   UI Viewport の Canvas ガイド、2Dピッキング、UI Gizmo。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include "ViewportCommon.hpp"
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Components/UIElement.hpp>
#include <Engine/Scene/Components/UIRect.hpp>

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
    /// 親要素の矩形サイズ。アンカーはこれに対する割合。
    math::Vector2 parentSize = math::Vector2::ZERO;
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

UITransform2D ComposeUITransform(const UITransform2D& parent, const scene::Transform& local,
                                 const math::Vector2& parentSize)
{
    /// @note UI の scale.xy は矩形サイズであり、親サイズを子の移動量へ掛けると Editor と Play の
    ///       双方で子要素が親から大きく外れるため、親の位置・回転だけを合成しサイズは各要素の
    ///       scale.xy を使う。アンカー/ピボットの解釈は scene::ResolveUIRect() に一本化する
    ///       (式が 2 つに分かれると必ず食い違う)。
    const math::Vector2 localPos = { local.position.x, local.position.y };
    UITransform2D result{};
    result.position = parent.position + RotateUIVector(localPos, parent.rotationZ);
    result.rotationZ = parent.rotationZ + ExtractUIZRotation(local.rotation);
    result.parentSize = parentSize;
    return result;
}

/// 要素の矩形サイズ。画像は scale.xy、文字は UISystem が書き戻した実測値。
math::Vector2 UIElementSize(scene::GameObject& go)
{
    const math::Vector2 scaleSize = { go.transform.scale.x, go.transform.scale.y };
    if (go.GetComponent<scene::UIImage>()) return scaleSize;
    if (const auto* text = go.GetComponent<scene::UIText>()) {
        if (text->resolvedSize.x > 0.0f || text->resolvedSize.y > 0.0f)
            return text->resolvedSize;
    }
    return scaleSize;
}

scene::UIAnchor UIElementAnchoring(scene::GameObject& go)
{
    if (const auto* image = go.GetComponent<scene::UIImage>()) return image->anchoring;
    if (const auto* text = go.GetComponent<scene::UIText>())  return text->anchoring;
    return {};
}

/// 書き込み用。ストレッチ軸のリサイズは scale ではなく余白を動かすので、
/// ギズモは実体へ書き戻す必要がある。
scene::UIAnchor* UIElementAnchoringMutable(scene::GameObject& go)
{
    if (auto* image = go.GetComponent<scene::UIImage>()) return &image->anchoring;
    if (auto* text = go.GetComponent<scene::UIText>())   return &text->anchoring;
    return nullptr;
}

/// @brief ドラッグ開始時点の余白。ストレッチ軸のリサイズはここからの差分で作り直す。
/// @note ファイルスコープなのは、掴めるのが 1 要素の 1 ハンドルだけで寿命がマウスを離すまでのため。
///       呼び出し側へ引数を 2 本足すほどの状態ではない。
math::Vector2 s_dragStartOffsetMax{};

/// @name 兄弟へのスナップ
struct UISnapGuide {
    math::Vector2 from{};
    math::Vector2 to{};
};

struct UISnapResult {
    math::Vector2 position{};   ///< 吸着後のローカル位置
    UISnapGuide   guides[2]{};  ///< 縦・横で最大 1 本ずつ
    int           guideCount = 0;
};

/// @brief 兄弟要素の 左/中央/右 (と 上/中央/下) の線へ、画面上で一定距離まで近づいたら吸着する。
/// @note 画面距離で判定するのは、Canvas 座標の固定値だと拡大表示で吸着範囲が広すぎ縮小表示では
///       狭すぎるため。掴んでいるのは画面上のマウスなので、感度も画面基準で決める。
UISnapResult SnapToSiblings(scene::GameObject& go,
                            const UITransform2D& parentResolved,
                            const scene::UIAnchor& anchoring,
                            const math::Vector2& size,
                            const math::Vector2& localPosition,
                            float scaleX,
                            float scaleY)
{
    UISnapResult result{};
    result.position = localPosition;

    scene::GameObject* parent = go.GetParent();
    if (!parent) return result;

    /// @note 吸着の許容量 (画面 8px 相当) を Canvas 単位へ戻す。
    const float tolX = scaleX > 0.0001f ? 8.0f / scaleX : 0.0f;
    const float tolY = scaleY > 0.0001f ? 8.0f / scaleY : 0.0f;

    const scene::UIRect self = scene::ResolveUIRect(
        parentResolved.parentSize, localPosition, size, anchoring);

    float bestDX = tolX, bestDY = tolY;
    bool  hasX = false, hasY = false;
    math::Vector2 guideX0{}, guideX1{}, guideY0{}, guideY1{};

    for (int i = 0; i < parent->GetChildCount(); ++i) {
        scene::GameObject* sibling = parent->GetChild(i);
        if (!sibling || sibling == &go || !sibling->activeInHierarchy()) continue;

        const math::Vector2 siblingSize = UIElementSize(*sibling);
        if (siblingSize.x <= 0.0f && siblingSize.y <= 0.0f) continue;
        const scene::UIRect other = scene::ResolveUIRect(
            parentResolved.parentSize,
            { sibling->transform.position.x, sibling->transform.position.y },
            siblingSize, UIElementAnchoring(*sibling));

        /// @note 左端どうし / 中央どうし / 右端どうし。相手の辺に自分のどの辺を合わせるかは
        ///       同じ種類どうしだけにする (左端を相手の右端へ吸わせると意図しない重なりが増える)。
        const float selfX[3]  = { self.position.x,  self.position.x  + self.size.x  * 0.5f,
                                  self.position.x  + self.size.x };
        const float otherX[3] = { other.position.x, other.position.x + other.size.x * 0.5f,
                                  other.position.x + other.size.x };
        for (int k = 0; k < 3; ++k) {
            const float d = otherX[k] - selfX[k];
            if (std::abs(d) < std::abs(bestDX)) {
                bestDX = d; hasX = true;
                const float top = (std::min)(self.position.y, other.position.y);
                const float bot = (std::max)(self.position.y + self.size.y,
                                             other.position.y + other.size.y);
                guideX0 = { otherX[k], top };
                guideX1 = { otherX[k], bot };
            }
        }

        const float selfY[3]  = { self.position.y,  self.position.y  + self.size.y  * 0.5f,
                                  self.position.y  + self.size.y };
        const float otherY[3] = { other.position.y, other.position.y + other.size.y * 0.5f,
                                  other.position.y + other.size.y };
        for (int k = 0; k < 3; ++k) {
            const float d = otherY[k] - selfY[k];
            if (std::abs(d) < std::abs(bestDY)) {
                bestDY = d; hasY = true;
                const float left  = (std::min)(self.position.x, other.position.x);
                const float right = (std::max)(self.position.x + self.size.x,
                                               other.position.x + other.size.x);
                guideY0 = { left,  otherY[k] };
                guideY1 = { right, otherY[k] };
            }
        }
    }

    if (hasX) {
        result.position.x += bestDX;
        result.guides[result.guideCount++] = { guideX0, guideX1 };
    }
    if (hasY) {
        result.position.y += bestDY;
        result.guides[result.guideCount++] = { guideY0, guideY1 };
    }
    return result;
}

void GetCanvasEditorSize(const EditorContext& ctx, float& canvasW, float& canvasH);

UITransform2D ResolveUITransform(const EditorContext& ctx, scene::GameObject& go, bool includeSelf)
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
    /// @note Canvas 直下の子から見た「親」は Canvas そのもの (0 のままだとアンカーが左上へ潰れる)。
    ///       canvasWidth を直接読まないのは、ScaleWithScreenSize では実際に見える Canvas 範囲が
    ///       viewport/scale で決まり canvasHeight と一致しないため。GetCanvasEditorSize() と同じ出所へ揃える。
    if (current && current->GetComponent<scene::UICanvas>()) {
        float canvasW = 1920.0f, canvasH = 1080.0f;
        GetCanvasEditorSize(ctx, canvasW, canvasH);
        resolved.parentSize = { canvasW, canvasH };
    }
    /// @note Canvas 直下から降りる。親サイズは 1 つ上の要素の矩形。
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const math::Vector2 parentSize = resolved.parentSize;
        resolved = ComposeUITransform(resolved, (*it)->transform, parentSize);
        resolved.parentSize = UIElementSize(**it);
    }
    return resolved;
}

const scene::UICanvas* FindCanvasEditorCanvas(const EditorContext& ctx)
{
    if (!ctx.activeScene) return nullptr;

    /// @note Canvas Editor は ScreenSpace UI を編集するビューとして扱う。activeUICanvas → 選択中
    ///       Canvas → 最初の ScreenSpace 系 Canvas の順に決め、複数 Canvas の編集対象が
    ///       フレームごとに揺れないようにする。
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
    /// @note UISystem::ResolveCanvasScale と同じ式で Game RT 上の論理 Canvas 範囲を求める。式が
    ///       分かれると、UI Viewport で合わせた位置が Play 開始時にずれて見える。
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
        /// @note Canvas Scaler が効くのは Overlay だけ。ScreenSpaceCamera はカメラ前方 planeDistance に
        ///       置いた canvasWidth x canvasHeight の板で、広さは画面と関係が無い (Engine 側
        ///       ResolveCanvasRectSize と同じ切り分け)。ここでスケーラーを掛けると編集中の矩形だけ実描画とずれる。
        if (canvas->renderMode == scene::UIRenderMode::ScreenSpaceOverlay &&
            canvas->scaleMode == scene::UICanvasScaleMode::ScaleWithScreenSize) {
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

    /// @note Canvas 外周・中央線・Safe Area 目安を描く。UI 実描画は RT 側、編集補助は ImGui 側に
    ///       分離することで、Game 出力へガイドが混入しない。
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

/// Pick the topmost UI element clicked in the UI viewport.
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
        /// @note 親ごと無効化された UI は描かれないので掴ませない (見えない要素が手前の要素より先に選ばれる)。
        if (!go.activeInHierarchy())
            continue;

        if (auto* canvas = go.GetComponent<scene::UICanvas>();
            canvas && IsCanvasEditorCanvas(*canvas)) {
            /// @note 当たりは canvasWidth ではなく、いま見えている Canvas 範囲。
            ///       cx / cy がこの空間の値なので、素の canvasHeight と比べると
            ///       ScaleWithScreenSize で参照解像度と違うアスペクトのとき下端がずれる。
            if (cx >= 0.0f && cx <= canvasW && cy >= 0.0f && cy <= canvasH) {
                const float area = canvasW * canvasH;
                if (area < bestArea) { bestArea = area; best = go.GetID(); }
            }
            continue;
        }
        if (!scene::IsUIElement(go)) continue;

        /// @note 当たり矩形はギズモが描くものと同じ式で解く。ここだけ「resolved.position = 矩形の左上」
        ///       で当てていたため、アンカーやピボットを動かすと絵と当たり判定が別の場所に居た。
        const UITransform2D parentResolved = ResolveUITransform(ctx, go, false);
        const UITransform2D resolved =
            ComposeUITransform(parentResolved, go.transform, parentResolved.parentSize);
        const scene::UIRect rect = scene::ResolveUIRect(
            parentResolved.parentSize, resolved.position,
            UIElementSize(go), UIElementAnchoring(go));

        /// @note 絵を持たない要素は寸法が 0 のまま置かれることがある (空の入れ物として
        ///       置いた Mask など)。面積が無いと二度と掴めないので、代わりに基準点の
        ///       まわりを当たりにして Hierarchy 以外からも選べるようにする。
        if (rect.size.x <= 0.0f || rect.size.y <= 0.0f) {
            constexpr float kHitR = 20.0f;
            const math::Vector2 point = rect.PointAt({ 0.5f, 0.5f });
            if (std::abs(cx - point.x) < kHitR && std::abs(cy - point.y) < kHitR) {
                if (0.0f < bestArea) { bestArea = 0.0f; best = go.GetID(); }
            }
            continue;
        }

        if (rect.Contains({ cx, cy })) {
            const float area = rect.size.x * rect.size.y;
            if (area < bestArea) { bestArea = area; best = go.GetID(); }
        }
    }

    if (!best.IsValid()) {
        if (!ImGui::GetIO().KeyCtrl) ClearEntitySelection(ctx);
    } else {
        if (ImGui::GetIO().KeyCtrl) AddToSelection(ctx, best);
        else                        SelectEntity(ctx, best);
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

/// @name UI gizmo helpers

/// Colors.
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
/// アンカー / ピボットは移動ハンドル (シアン) と色で切り分ける。
/// 掴めるものと、状態を示すだけのものが同じ色だと触れると思って空振りする。
constexpr ImU32 kUIColAnchor     = IM_COL32(255, 190,  60, 230);
constexpr ImU32 kUIColAnchorLink = IM_COL32(255, 190,  60, 110);
constexpr ImU32 kUIColPivot      = IM_COL32(255, 120, 200, 230);
/// 吸着した線。一時的に出るだけなので、他のどれとも違う色にして見落とさないようにする。
constexpr ImU32 kUIColSnap       = IM_COL32(120, 255, 160, 220);
constexpr float kUIArrowLen   = 55.0f;  ///< screen px
constexpr float kUIHandleR    = 5.0f;
constexpr float kUICenterR    = 6.0f;

/// @brief 選択中の UI 要素の矩形をまとめて描く。
/// @note ギズモは選択の先頭 1 つしか面倒を見ないため複数選択を可視化する。UI Viewport は
///       Game View の完成済み RT を映すため Engine の選択マスクは使えず、ScreenSpace UI の
///       輪郭は ImGui 側にしか置けない (WorldSpace は Scene View の 3D マスクが拾う)。
void DrawUISelectionOutlines(EditorContext& ctx, const ImVec2& viewportMin, const ImVec2& viewportSize)
{
    if (!ctx.activeScene) return;

    float canvasW = 1920.0f, canvasH = 1080.0f;
    GetCanvasEditorSize(ctx, canvasW, canvasH);
    const float scaleX = viewportSize.x / canvasW;
    const float scaleY = viewportSize.y / canvasH;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const scene::EntityID id : ctx.selectedEntities) {
        scene::GameObject* go = ctx.activeScene->GetGameObject(id);
        if (!go || !scene::IsUIElement(*go)) continue;
        /// @note 別 Canvas の要素は今の Canvas の縮尺で描くと違う場所に出る。
        if (!IsUnderCanvas(*go, ctx.activeUICanvas)) continue;

        const UITransform2D parentResolved = ResolveUITransform(ctx, *go, false);
        const UITransform2D resolved =
            ComposeUITransform(parentResolved, go->transform, parentResolved.parentSize);
        const math::Vector2 size = UIElementSize(*go);
        if (size.x <= 0.0f || size.y <= 0.0f) continue;
        const scene::UIRect rect = scene::ResolveUIRect(
            parentResolved.parentSize, resolved.position, size, UIElementAnchoring(*go));

        const math::Vector2 center = rect.Center();
        const ImVec2 centerScr = { viewportMin.x + center.x * scaleX,
                                   viewportMin.y + center.y * scaleY };
        const float cosZ = std::cosf(resolved.rotationZ);
        const float sinZ = std::sinf(resolved.rotationZ);
        const float hW = size.x * 0.5f, hH = size.y * 0.5f;
        const auto corner = [&](float lx, float ly) -> ImVec2 {
            return { centerScr.x + (lx * cosZ - ly * sinZ) * scaleX,
                     centerScr.y + (lx * sinZ + ly * cosZ) * scaleY };
        };
        const ImVec2 c[4] = { corner(-hW, -hH), corner(hW, -hH), corner(hW, hH), corner(-hW, hH) };

        /// @note 文字は実測なので破線にして「掴んで広げられる矩形ではない」ことを示す。
        if (scene::HasMeasuredUISize(*go)) {
            for (int i = 0; i < 4; ++i) {
                const ImVec2 a = c[i], b = c[(i + 1) % 4];
                constexpr int kDashes = 8;
                for (int d = 0; d < kDashes; d += 2) {
                    const float t0 = static_cast<float>(d) / kDashes;
                    const float t1 = static_cast<float>(d + 1) / kDashes;
                    dl->AddLine({ a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0 },
                                { a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1 },
                                kUIColOutline, 1.5f);
                }
            }
        } else {
            dl->AddQuad(c[0], c[1], c[2], c[3], kUIColOutline, 1.5f);
        }
    }
}

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

/// 2D gizmo for UI viewport. Uses the same Q/W/E/R controls as the 3D gizmo.
///   W = translate, with constrained X/Y axes and free center drag
///   E = rotate around Z using the ring
///   R = resize with 8 handles (実測サイズの文字を除く)
///   Q = toggle World/Local space
/// Returns true when the gizmo consumed mouse input.
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

    scene::GameObject* go = ctx.GetSelectedGO();
    if (!go || !ctx.activeScene) return false;

    if (!scene::IsUIElement(*go)) return false;
    /// @note 掴んで広げられるか (実測サイズの文字だけは広げられない)。矩形の描き方と
    ///       R のハンドルの有無はこれ 1 つで決まる。
    const bool measuredSize = scene::HasMeasuredUISize(*go);

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
    const UITransform2D parentResolved = ResolveUITransform(ctx, *go, false);
    const UITransform2D resolved =
        ComposeUITransform(parentResolved, t, parentResolved.parentSize);
    const float localPx = t.position.x, localPy = t.position.y;

    /// @note 矩形は Engine と同じ 1 本の式で解く。以前は Editor 側だけ「position = 矩形の左上」を
    ///       前提にしていたため、sw/sh が 0 のまま原点に十字を描くだけで、アンカーを変えても
    ///       表示は一切変わらなかった。
    const scene::UIAnchor anchoring = UIElementAnchoring(*go);
    const math::Vector2   elementSize = UIElementSize(*go);
    const math::Vector2   parentSize  = parentResolved.parentSize;
    const scene::UIRect   rect = scene::ResolveUIRect(
        parentSize, resolved.position, elementSize, anchoring);
    const float px = rect.position.x, py = rect.position.y;
    const float sw = elementSize.x;
    const float sh = elementSize.y;

    /// @note Extract Z rotation from the quaternion.
    const math::Quaternion& q = t.rotation;
    const float localZAngle = ExtractUIZRotation(q);
    const float zAngle = resolved.rotationZ;
    const float cosZ = std::cosf(zAngle), sinZ = std::sinf(zAngle);

    /// @note Rectangle center in canvas and screen space.
    const float cenCX = px + sw * 0.5f, cenCY = py + sh * 0.5f;
    const ImVec2 centerScr = toScreen(cenCX, cenCY);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool wantsMouse = false;

    /// @note drag stores the active UI gizmo operation across frames:
    ///       -1  = none
    ///       0  = free translate from center
    ///       1  = constrained X-axis translate
    ///       2  = constrained Y-axis translate
    ///       3-10 = resize handles 0-7
    ///       20  = rotation ring
    const EditorContext::GizmoMode mode = ctx.gizmoMode;

    /// @note Rotated rectangle outline.
    auto rotOfs = [&](float lx, float ly) -> ImVec2 {
        return {
            centerScr.x + (lx*cosZ - ly*sinZ) * scaleX,
            centerScr.y + (lx*sinZ + ly*cosZ) * scaleY
        };
    };
    /// @note 選択中の矩形は DrawUISelectionOutlines がまとめて描く。ここは掴めるものだけ。

    /// @name アンカーとピボット
    /// @note 「親のどこを基準にしているか」と「自分のどこがその点に合っているか」は別で、
    ///       片方だけでは要素が動く理由が分からない。2 点を線で結ぶとローカル位置がその
    ///       線の長さだとひと目で読める。
    {
        const math::Vector2 anchorCanvas = {
            parentResolved.position.x + parentSize.x * anchoring.anchor.x,
            parentResolved.position.y + parentSize.y * anchoring.anchor.y,
        };
        const ImVec2 anchorScr = toScreen(anchorCanvas.x, anchorCanvas.y);
        const math::Vector2 pivotCanvas = rect.PointAt(anchoring.pivot);
        const ImVec2 pivotScr = toScreen(pivotCanvas.x, pivotCanvas.y);

        /// @note アンカー: 親の中の基準点。折れた十字で「親側の点」だと分かるようにする。
        constexpr float kAnchorArm = 7.0f;
        dl->AddLine({ anchorScr.x - kAnchorArm, anchorScr.y }, { anchorScr.x + kAnchorArm, anchorScr.y },
                    kUIColAnchor, 1.5f);
        dl->AddLine({ anchorScr.x, anchorScr.y - kAnchorArm }, { anchorScr.x, anchorScr.y + kAnchorArm },
                    kUIColAnchor, 1.5f);

        /// @note 基準点 → ピボット。これがローカル位置そのもの。
        const float dx = pivotScr.x - anchorScr.x, dy = pivotScr.y - anchorScr.y;
        if (dx * dx + dy * dy > 4.0f)
            dl->AddLine(anchorScr, pivotScr, kUIColAnchorLink, 1.0f);

        /// @note ピボット: 回転と拡縮の中心でもある点。中空の丸で矩形上の点だと示す。
        dl->AddCircleFilled(pivotScr, 3.5f, kUIColPivot);
        dl->AddCircle(pivotScr, 5.5f, kUIColPivot, 0, 1.5f);
    }

    /// @note Local axes converted to unit vectors in screen space.
    auto screenUnitAxis = [&](float ax, float ay) -> ImVec2 {
        ImVec2 v = { ax * scaleX, ay * scaleY };
        const float len = std::sqrt(v.x*v.x + v.y*v.y);
        return (len > 0.001f) ? ImVec2{ v.x/len, v.y/len } : ImVec2{ 1, 0 };
    };
    const ImVec2 dirX = screenUnitAxis( cosZ,  sinZ);
    const ImVec2 dirY = screenUnitAxis( sinZ, -cosZ);
    const ImVec2 tipX = { centerScr.x + dirX.x * kUIArrowLen, centerScr.y + dirX.y * kUIArrowLen };
    const ImVec2 tipY = { centerScr.x + dirY.x * kUIArrowLen, centerScr.y + dirY.y * kUIArrowLen };

    /// @note W: translate mode.
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

        /// @note 矩形の寸法がまだ無いときだけ、基準点に十字を出す。サイズが 0 だと矩形が線に潰れて
        ///       選択できているのか分からないため、「まだ測れていない/サイズを入れないと掴めない」の
        ///       合図として残す。
        if (sw <= 0.0f || sh <= 0.0f) {
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
            /// @note Ctrl 押下中はモーメンタリスナップ (Unity 互換)
            if (ctx.snapEnabled || ImGui::GetIO().KeyCtrl) { newX = std::round(newX); newY = std::round(newY); }

            /// @note 兄弟要素の辺・中心へ吸着する。1px 丸めだけでは「隣のパネルと左端が揃っているか」
            ///       は分からず、UI で揃えたいのは座標のキリの良さでなく他要素との関係。Alt で
            ///       切れるようにするのは、吸着があると「あと 1px だけずらす」最後の詰めができないため。
            UISnapResult snap{};
            if (!ImGui::GetIO().KeyAlt) {
                snap = SnapToSiblings(*go, parentResolved, anchoring, elementSize,
                                      { newX, newY }, scaleX, scaleY);
                newX = snap.position.x;
                newY = snap.position.y;
            }
            t.position.x = newX;
            t.position.y = newY;

            /// @note 吸着した線を出す。何に揃ったのかが見えないと、狙って使えない。
            for (int i = 0; i < snap.guideCount; ++i) {
                const UISnapGuide& guide = snap.guides[i];
                const ImVec2 a = toScreen(guide.from.x, guide.from.y);
                const ImVec2 b = toScreen(guide.to.x, guide.to.y);
                dl->AddLine(a, b, kUIColSnap, 1.0f);
            }
        }
    }
    /// @note E: rotate mode.
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

        /// @note Angle indicator.
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
            if (ctx.snapEnabled || ImGui::GetIO().KeyCtrl) {
                constexpr float k15deg = 3.14159265f / 12.0f;
                newZ = std::round(newZ / k15deg) * k15deg;
            }
            t.rotation = math::Quaternion::FromEuler({ 0.0f, 0.0f, newZ });
        }

        /// @note Angle label.
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f deg", zAngle * (180.0f / 3.14159265f));
        dl->AddText({ centerScr.x + ringR + 6.0f, centerScr.y - 7.0f },
                    IM_COL32(200, 220, 255, 200), buf);
    }
    /// @note R: resize. 実測サイズの文字以外は transform.scale.xy が矩形そのもの。
    else if (mode == EditorContext::GizmoMode::Scale && !measuredSize) {
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
                /// @note ストレッチ軸は余白を動かすので、その開始値も控える。ドラッグ中は毎フレーム
                ///       「掴んだ時点からの総移動量」で作り直す (差分を積み上げるとフレーム数に
                ///       比例して滑る)。
                if (const scene::UIAnchor* a = UIElementAnchoringMutable(*go))
                    s_dragStartOffsetMax = a->offsetMax;
            }
        }

        if (drag >= 3 && drag <= 10 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 mm = ImGui::GetMousePos();
            const float dcx = (mm.x - dragStart.x) / scaleX;
            const float dcy = (mm.y - dragStart.y) / scaleY;
            /// @note Project mouse delta onto local axes.
            const float projX =  dcx * cosZ + dcy * sinZ;
            const float projY = -dcx * sinZ + dcy * cosZ;

            const int hi = drag - 3;
            float nw = startWidth, nh = startHeight;
            /// @note left
            if (hi == 0 || hi == 3 || hi == 5) nw = startWidth - projX;
            /// @note right
            if (hi == 2 || hi == 4 || hi == 7) nw = startWidth + projX;
            /// @note top
            if (hi == 0 || hi == 1 || hi == 2) nh = startHeight - projY;
            /// @note bottom
            if (hi == 5 || hi == 6 || hi == 7) nh = startHeight + projY;
            nw = (std::max)(1.0f, nw);
            nh = (std::max)(1.0f, nh);
            if (ctx.snapEnabled || ImGui::GetIO().KeyCtrl) { nw = std::round(nw); nh = std::round(nh); }

            /// @note ストレッチしている軸では幅は親から決まるため scale を書いても次のフレームに
            ///       戻される。動かすべきは「どちら側の余白か」で、掴んだ辺と同じ側の余白を
            ///       動かすのが掴んだ場所と結果が一致する唯一の対応 (中央ハンドルは幅を変えない)。
            scene::UIAnchor* liveAnchor = UIElementAnchoringMutable(*go);
            const bool stretchX = liveAnchor && liveAnchor->stretchX;
            const bool stretchY = liveAnchor && liveAnchor->stretchY;
            const bool grabLeft   = (hi == 0 || hi == 3 || hi == 5);
            const bool grabRight  = (hi == 2 || hi == 4 || hi == 7);
            const bool grabTop    = (hi == 0 || hi == 1 || hi == 2);
            const bool grabBottom = (hi == 5 || hi == 6 || hi == 7);

            if (stretchX) {
                /// @note 幅の変化ぶんを、掴んだ側の余白へそのまま移す。
                if (grabLeft)
                    t.position.x = startX + (startWidth - nw);
                else if (grabRight)
                    liveAnchor->offsetMax.x = s_dragStartOffsetMax.x + (startWidth - nw);
            } else {
                /// @note ハンドルでの拡縮は矩形の中心を動かさない。
                ///
                ///       矩形中心 = 基準点 + ローカル位置 + サイズ × (0.5 - pivot) なので、
                ///       中心を保つには サイズの変化ぶん × (0.5 - pivot) をローカル位置へ戻す。
                ///       pivot = (0,0) では従来の式 (サイズ変化の半分) と一致する。
                t.scale.x = nw;
                t.position.x = startX + (startWidth - nw) * (0.5f - anchoring.pivot.x);
            }

            if (stretchY) {
                if (grabTop)
                    t.position.y = startY + (startHeight - nh);
                else if (grabBottom)
                    liveAnchor->offsetMax.y = s_dragStartOffsetMax.y + (startHeight - nh);
            } else {
                t.scale.y = nh;
                t.position.y = startY + (startHeight - nh) * (0.5f - anchoring.pivot.y);
            }
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

/// @brief 矢印キーで選択中 UI 要素を微移動する。
/// @note マウスドラッグはピクセル単位の位置合わせには細かすぎるため、矢印キー nudge
///       (1px、Shift で 10px) で Inspector の数値入力を介さず微調整できるようにする。
void HandleUINudge(EditorContext& ctx, bool allowed)
{
    if (!allowed || !ctx.activeScene)
        return;
    /// @note テキスト入力中（リネーム・数値入力等）は矢印キーをそちらへ譲る。
    if (ImGui::GetIO().WantTextInput)
        return;

    scene::GameObject* go = ctx.GetSelectedGO();
    if (!go || !scene::IsUIElement(*go))
        return;

    /// @note 離散押下のみ拾う。repeat を許可すると押しっぱなしで毎フレーム undo 履歴が積もるため、
    ///       1 タップ = 1 nudge = 1 undo に保つ。
    float dx = 0.0f, dy = 0.0f;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  /*repeat=*/false)) dx -= 1.0f;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, /*repeat=*/false)) dx += 1.0f;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow,    /*repeat=*/false)) dy -= 1.0f;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow,  /*repeat=*/false)) dy += 1.0f;
    if (dx == 0.0f && dy == 0.0f)
        return;

    const float step = ImGui::GetIO().KeyShift ? 10.0f : 1.0f;
    const scene::Transform before = go->transform;
    go->transform.position.x += dx * step;
    go->transform.position.y += dy * step;
    const scene::Transform after = go->transform;

    const std::string instanceId = go->instanceId;
    scene::Scene* scene = ctx.activeScene;
    const std::function<void()> markDirty = ctx.markSceneDirty;
    auto apply = [scene, instanceId, markDirty](const scene::Transform& value) {
        if (auto* target = scene->FindByGuid(instanceId)) {
            target->transform = value;
            if (markDirty)
                markDirty();
        }
    };
    if (ctx.undoStack) {
        ctx.undoStack->Push(std::make_unique<LambdaCommand>(
            "Nudge UI Element",
            [apply, after]()  { apply(after); },
            [apply, before]() { apply(before); }));
    }
    if (markDirty)
        markDirty();
}


} // namespace fbzz::editor
