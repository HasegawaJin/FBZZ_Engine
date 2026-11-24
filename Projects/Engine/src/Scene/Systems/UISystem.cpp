/// @file    UISystem.cpp
/// @brief   CPU で確定した UI 矩形の GPU 描画と互換呼び出しの接続。
/// @author  Hasegawa Jin
/// @date    2026-05-23

#include "UIRendering.hpp"
#include "UIRuntimeGeometry.hpp"

#include <Engine/Renderer/Camera.hpp>
#include <algorithm>
#include <vector>

namespace fbzz::scene {
namespace {

using namespace ui_runtime_detail;
using namespace ui_render_detail;

struct CanvasRuntimeState {
    math::Matrix4 canvasToClip = math::Matrix4::Identity();
    renderer::ResourceHandle<renderer::PipelineStateTag> pipelineState;
    renderer::RenderLayer layer = renderer::RenderLayer::OVERLAY_LAYER;
};

void BeginUIFrame(UISystemContext& context)
{
    /// @note 呼び出しごとにクリップ階層を空の状態から始める。
    context.clipPlanes.clear();
}

CanvasRuntimeState BuildCanvasRuntimeState(const UICanvas& canvas,
                                           const GameObject& canvasObject,
                                           float viewportWidth,
                                           float viewportHeight,
                                           const math::Matrix4& viewProjection,
                                           math::Vector3 cameraWorldPos,
                                           math::Quaternion cameraWorldRot,
                                           const UISystemContext& context)
{
    CanvasRuntimeState state{};

    /// @name WorldSpace
    if (canvas.renderMode == UIRenderMode::WorldSpace) {
        const float worldUnitsPerPixel = canvas.worldScale;

        math::Matrix4 pixelToLocal = math::Matrix4::Identity();
        pixelToLocal.m[0][0] =  worldUnitsPerPixel;
        pixelToLocal.m[1][1] = -worldUnitsPerPixel;
        pixelToLocal.m[0][3] = -canvas.canvasWidth  * 0.5f * worldUnitsPerPixel;
        pixelToLocal.m[1][3] =  canvas.canvasHeight * 0.5f * worldUnitsPerPixel;

        /// @note Canvas が子 GO に置かれることがあるので worldPosition を使う。
        /// @note faceCamera は向きだけカメラ姿勢で上書きする (位置は追従したまま正対する)。
        const math::Quaternion canvasRotation =
            canvas.faceCamera ? cameraWorldRot : canvasObject.transform.worldRotation;
        const math::Matrix4 worldMatrix = math::Matrix4::TRS(
            canvasObject.transform.worldPosition - canvasRotation * math::Vector3::FORWARD * canvas.surfaceOffset,
            canvasRotation,
            math::Vector3::ONE
        );

        state.canvasToClip = viewProjection * worldMatrix * pixelToLocal;
        state.pipelineState = canvas.depthMode == UICanvasDepthMode::ALWAYS_ON_TOP ? context.pso : context.worldPso;
        state.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
        return state;
    }

    /// @name ScreenSpaceCamera
    if (canvas.renderMode == UIRenderMode::ScreenSpaceCamera) {
        /// @note Canvas をカメラ前方 planeDistance に置いてカメラ向きで固定する。
        /// @note Overlay と同じスクリーン UI に深度テストを足す用途。マウス座標系は Overlay と同じ。
        const float worldUnitsPerPixel = canvas.worldScale;

        math::Matrix4 pixelToLocal = math::Matrix4::Identity();
        pixelToLocal.m[0][0] =  worldUnitsPerPixel;
        pixelToLocal.m[1][1] = -worldUnitsPerPixel;
        pixelToLocal.m[0][3] = -canvas.canvasWidth  * 0.5f * worldUnitsPerPixel;
        pixelToLocal.m[1][3] =  canvas.canvasHeight * 0.5f * worldUnitsPerPixel;

        /// @note カメラ前方ベクトル: worldRot * (0, 0, 1)
        const math::Vector3 cameraForward = cameraWorldRot * math::Vector3{ 0.0f, 0.0f, 1.0f };
        const math::Vector3 canvasWorldPosition = cameraWorldPos + cameraForward * (canvas.planeDistance - canvas.surfaceOffset);
        const math::Matrix4 worldMatrix = math::Matrix4::TRS(
            canvasWorldPosition, cameraWorldRot, math::Vector3::ONE);

        state.canvasToClip = viewProjection * worldMatrix * pixelToLocal;
        /// @note 深度テストあり (3D オブジェクトに遮蔽可)
        state.pipelineState = canvas.depthMode == UICanvasDepthMode::ALWAYS_ON_TOP ? context.pso : context.worldPso;
        state.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
        return state;
    }

    /// @name ScreenSpaceOverlay (デフォルト)
    float visibleCanvasWidth = 1.0f;
    float visibleCanvasHeight = 1.0f;
    ResolveScreenSpaceCanvasArea(canvas, viewportWidth, viewportHeight,
                                 visibleCanvasWidth, visibleCanvasHeight);

    state.canvasToClip = math::Matrix4::Orthographic(
        0.0f, visibleCanvasWidth,
        visibleCanvasHeight, 0.0f,
        0.0f, 1.0f);
    state.pipelineState = context.pso;
    state.layer = renderer::RenderLayer::OVERLAY_LAYER;
    return state;
}

/// @name デバッグ表示
/// @note 矩形・アンカー・ピボットを白テクスチャの細い四角で重ねる。
/// @note 描くのは RectFromTransform が返した矩形そのもの。別に計算した図だと、ずれたときに
/// @note 「表示のバグ」なのか「判定のバグ」なのか分からない。
void SubmitDebugRect(const CanvasDrawContext& draw,
                     const math::Vector2& position,
                     const math::Vector2& size,
                     const math::Vector4& color,
                     float thickness)
{
    const auto drawEdge = [&](float left, float top, float width, float height) {
        if (width <= 0.0f || height <= 0.0f) return;
        SubmitSolidQuad(draw, { left, top }, { width, height }, color);
    };
    drawEdge(position.x, position.y, size.x, thickness);
    drawEdge(position.x, position.y + size.y - thickness, size.x, thickness);
    drawEdge(position.x, position.y, thickness, size.y);
    drawEdge(position.x + size.x - thickness, position.y, thickness, size.y);
}

void SubmitDebugMarker(const CanvasDrawContext& draw,
                       const math::Vector2& point,
                       const math::Vector4& color,
                       float radius)
{
    SubmitSolidQuad(draw, { point.x - radius, point.y - radius }, { radius * 2.0f, radius * 2.0f }, color);
}

/// @name レンダリング
/// @note 選択中の要素の矩形を、選択マスクへ白で塗る。
/// @note sortOrder を無視するのは、マスクが被覆だけを表すので重なりの前後が結果に出ないため。
/// @note 絵ではなく矩形を塗るのは、アルファでシルエットを取ると暗い絵や絵を持たない
/// @note Mask / Scroll View がマスクに出ないため。
void SubmitSelectionMaskRecursive(GameObject& element,
                                  const UITransform2D& parentTransform,
                                  const CanvasDrawContext& draw,
                                  const std::function<bool(GameObject&)>& isSelected,
                                  bool applySelfTransform)
{
    if (!element.activeInHierarchy()) return;

    const UITransform2D elementTransform = applySelfTransform
        ? ComposeUITransform(parentTransform, element.transform, parentTransform.parentSize)
        : parentTransform;
    const Rect rect = RectFromTransform(element, elementTransform);

    /// @note Canvas 自身 (applySelfTransform == false) は塗らない。選ぶと画面全体が
    /// @note 囲われるだけで、どの要素を見ているのか分からなくなる。
    if (applySelfTransform && rect.size.x > 0.0f && rect.size.y > 0.0f && isSelected(element)) {
        SubmitSolidQuad(draw, rect.pos, rect.size, { 1.0f, 1.0f, 1.0f, 1.0f }, elementTransform.rotationZ);
    }

    UITransform2D childTransform = elementTransform;
    /// @note Canvas ルートでは上書きしない (ProcessUIEventsRecursive と同じ理由)。
    if (applySelfTransform)
        childTransform.parentSize = rect.size;
    if (const auto* scroll = element.GetComponent<UIScrollView>(); scroll && scroll->enabled)
        childTransform.position -= RotateUIVector(scroll->scrollPosition, elementTransform.rotationZ);

    for (int childIndex = 0; childIndex < element.GetChildCount(); ++childIndex) {
        if (GameObject* child = element.GetChild(childIndex))
            SubmitSelectionMaskRecursive(*child, childTransform, draw, isSelected, true);
    }
}

/// @param groupOpacity 上位の UICanvasGroup を掛け合わせた透明度。
void RenderCanvasRecursive(GameObject& element,
                           const UITransform2D& parentTransform,
                           const CanvasDrawContext& draw,
                           bool applySelfTransform,
                           std::size_t depth,
                           float groupOpacity,
                           const asset::UIThemeStyle* theme)
{
    if (!element.activeInHierarchy()) return;
    auto& context = draw.context;

    /// @note 群の透明度は掛け合わせる。入れ子のフェードが互いを打ち消さないため。
    if (const auto* group = element.GetComponent<UICanvasGroup>(); group && group->enabled) {
        groupOpacity = group->ignoreParentGroups
            ? std::clamp(group->alpha, 0.0f, 1.0f)
            : groupOpacity * std::clamp(group->alpha, 0.0f, 1.0f);
    }
    /// @note 完全に透明なら以下は 1 枚も出ない。子孫ごと降りずに済ませる。
    if (groupOpacity <= 0.0f) return;

    UITransform2D elementTransform = applySelfTransform
        ? ComposeUITransform(parentTransform, element.transform, parentTransform.parentSize)
        : parentTransform;
    auto* image  = element.GetComponent<UIImage>();
    auto* button = element.GetComponent<UIButton>();
    auto* text   = element.GetComponent<UIText>();

    /// @note 押下中はボタン全体を右下へ沈ませる。pressedColor だけだと暗い背景との乗算後に
    /// @note 差が小さく、入力が UIButton まで届いたのかを画面で判別できない。
    if (button && button->enabled && button->isInteractable
        && button->state == UIButtonState::PRESSED) {
        constexpr math::Vector2 PRESSED_VISUAL_OFFSET = { 2.0f, 2.0f };
        elementTransform.position += RotateUIVector(PRESSED_VISUAL_OFFSET, elementTransform.rotationZ);
    }

    if (image && image->enabled) DrawImageElement(draw, element, *image, button, elementTransform, groupOpacity, theme);
    if (text && text->enabled) DrawTextElement(draw, *text, elementTransform, groupOpacity, theme);

    /// @note 判定に使っているのと同じ矩形を重ねる。図と判定を別に計算しない。
    if (context.showRects) {
        const Rect debugRect = RectFromTransform(element, elementTransform);
        if (debugRect.size.x > 0.0f && debugRect.size.y > 0.0f) {
            const UIAnchor anchoring = UIElementAnchoring(element);
            SubmitDebugRect(draw,
                            debugRect.pos, debugRect.size,
                            { 0.0f, 0.85f, 1.0f, 0.55f }, 1.0f);
            /// @note ピボット (自分のどこが基準点に合っているか)。Editor のギズモと同じ桃色。
            /// @note アンカーは親の矩形も一緒に見えていないと意味を持たないので実行時は描かない。
            /// @note 実行時に知りたいのは「判定に使われている矩形はどこか」だけ。
            SubmitDebugMarker(draw,
                              { debugRect.pos.x + debugRect.size.x * anchoring.pivot.x,
                                debugRect.pos.y + debugRect.size.y * anchoring.pivot.y },
                              { 1.0f, 0.47f, 0.78f, 0.9f }, 3.0f);
        }
    }

    UITransform2D childTransform = elementTransform;
    /// @note 子のアンカーは「この要素の矩形」に対する割合になる。
    /// @note Canvas ルートでは上書きしない (ProcessUIEventsRecursive と同じ理由)。
    if (applySelfTransform)
        childTransform.parentSize = RectFromTransform(element, elementTransform).size;
    if (const auto* scroll = element.GetComponent<UIScrollView>(); scroll && scroll->enabled)
        childTransform.position -= RotateUIVector(scroll->scrollPosition, elementTransform.rotationZ);

    /// @note Mask / Scroll View はここから下の描画を自分の矩形で切る。
    /// @note 自分自身は切らない (枠の絵を切ると縁が半分消える)。切るのは子孫だけ。
    std::size_t pushedPlaneCount = 0;
    if (applySelfTransform && ElementClipsChildren(element)) {
        const Rect clipRect = RectFromTransform(element, elementTransform);
        if (clipRect.size.x > 0.0f && clipRect.size.y > 0.0f)
            pushedPlaneCount = PushClipRect(context.clipPlanes, clipRect, elementTransform.rotationZ);
    }

    /// @note 参照で受ける。この並びは子の再帰処理が終わるまで読み続けるので、深さごとの
    /// @note 領域を借りたまま降りる。コピーすると確保が戻ってくる。
    std::vector<GameObject*>& children = AcquireChildScratch(context, depth);
    SortUIChildren(element, children);
    for (GameObject* child : children)
        RenderCanvasRecursive(*child, childTransform, draw, true, depth + 1, groupOpacity, theme);

    context.clipPlanes.resize(context.clipPlanes.size() - pushedPlaneCount);
}

void DrawCanvases(const std::vector<CanvasEntry>& canvases,
                    renderer::IRenderer& renderer,
                    renderer::ResourceManager& resources,
                    UISystemContext& context,
                    float viewportWidth,
                    float viewportHeight,
                    math::Vector3 cameraWorldPos,
                    math::Quaternion cameraWorldRot,
                    const math::Matrix4& viewProjection,
                    UIRenderTargetView targetView)
{
    for (const CanvasEntry& entry : canvases) {
        if (!ShouldRenderCanvas(*entry.canvas, targetView, context.sceneDepthAvailable))
            continue;

        const CanvasRuntimeState state = BuildCanvasRuntimeState(
            *entry.canvas, *entry.go,
            viewportWidth, viewportHeight,
            viewProjection, cameraWorldPos, cameraWorldRot, context);
        context.textPixelScale =
            ResolveCanvasPixelScale(*entry.canvas, viewportWidth, viewportHeight);
        const UITransform2D canvasRoot =
            BuildCanvasRootTransform(*entry.canvas, viewportWidth, viewportHeight);
        const math::Matrix4 renderCanvasToClip = context.reversedDepth
            ? renderer::ReverseClipDepth(state.canvasToClip) : state.canvasToClip;
        const CanvasDrawContext draw{ renderer, resources, context, renderCanvasToClip, state.pipelineState, state.layer };
        RenderCanvasRecursive(*entry.go, canvasRoot, draw, false, 0, 1.0f,
                              entry.theme ? &*entry.theme : nullptr);
    }
}

} /// @note namespace

void UISystemSetDefaultFontPath(UISystemContext& context, const std::string& basePath)
{
    context.defaultFontPath = basePath;
}

void UISystem(Scene& scene,
              renderer::IRenderer& renderer,
              renderer::ResourceManager& resources,
              UISystemContext& context,
              float viewportWidth,
              float viewportHeight,
              math::Vector2 mouseInViewport,
              bool mousePressed,
              math::Vector3    cameraWorldPos,
              math::Quaternion cameraWorldRot,
              const math::Matrix4& viewProjection,
              UIRenderTargetView targetView)
{
    if (targetView == UIRenderTargetView::GameViewport) {
        UpdateUIInteraction(scene, context, {viewportWidth, viewportHeight, mouseInViewport,
            viewProjection, cameraWorldPos, cameraWorldRot, context.gameplayInput, context.sceneDepthAvailable}, mousePressed);
    } else UpdateUILayout(scene, context, viewportWidth, viewportHeight);
    PrepareUIResources(context, resources);
    BeginUIFrame(context);

    if (!context.shader.IsValid() || !context.constants.IsValid() ||
        !context.pso.IsValid() || !context.worldPso.IsValid() || !context.whiteTexture.IsValid())
        return;

    std::vector<CanvasEntry> canvases;
    CollectCanvases(scene, canvases);

    DrawCanvases(canvases, renderer, resources, context, viewportWidth, viewportHeight,
                   cameraWorldPos, cameraWorldRot, viewProjection, targetView);
}

void UISelectionMaskSystem(Scene& scene,
                           renderer::IRenderer& renderer,
                           renderer::ResourceManager& resources,
                           UISystemContext& context,
                           float viewportWidth,
                           float viewportHeight,
                           const std::function<bool(GameObject&)>& isSelected,
                           math::Vector3    cameraWorldPos,
                           math::Quaternion cameraWorldRot,
                           const math::Matrix4& viewProjection,
                           UIRenderTargetView targetView)
{
    if (!isSelected) return;

    PrepareUIResources(context, resources);
    BeginUIFrame(context);

    if (!context.shader.IsValid() || !context.constants.IsValid() ||
        !context.selectionMaskPso.IsValid() || !context.whiteTexture.IsValid())
        return;

    /// @note レイアウトはまだ本描画の前なので、この時点の transform で解く。
    /// @note UILayoutGroup も UIText の実測も前フレームぶんが載っており、1 フレーム古い矩形に
    /// @note なるのは動いている最中だけ。
    std::vector<CanvasEntry> canvases;
    CollectCanvases(scene, canvases);

    for (const CanvasEntry& entry : canvases) {
        if (!ShouldRenderCanvas(*entry.canvas, targetView))
            continue;

        const CanvasRuntimeState state = BuildCanvasRuntimeState(
            *entry.canvas, *entry.go, viewportWidth, viewportHeight,
            viewProjection, cameraWorldPos, cameraWorldRot, context);

        const UITransform2D canvasRoot =
            BuildCanvasRootTransform(*entry.canvas, viewportWidth, viewportHeight);
        /// @note 選択マスク RT は Reversed-Z。スクリーン空間 (z=0) は最手前の 1 へ、
        /// @note ワールド空間はカメラの GPU 射影と同じ深度へ移る。
        const math::Matrix4 canvasToClip = renderer::ReverseClipDepth(state.canvasToClip);
        const CanvasDrawContext draw{ renderer, resources, context, canvasToClip, context.selectionMaskPso, state.layer };
        SubmitSelectionMaskRecursive(*entry.go, canvasRoot, draw, isSelected, false);
    }
}

} /// @note namespace fbzz::scene
