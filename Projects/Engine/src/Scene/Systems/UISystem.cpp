// FBZZ Engine
// UISystem.cpp | fbzz::scene
// ランタイム UI の描画とボタン入力処理
// UICanvas / UIImage / UIText / UIButton を走査し、DrawCall と hit 状態を作る。
// ScreenSpace / ScreenSpaceCamera / WorldSpace の 3 モードを扱う。
#include "Engine/Scene/Systems/UISystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Components/UICanvas.hpp"
#include "Engine/Scene/Components/UIImage.hpp"
#include "Engine/Scene/Components/UIButton.hpp"
#include "Engine/Scene/Components/UIText.hpp"
#include "Engine/Scene/Components/UILayoutGroup.hpp"
#include "Engine/Scene/Components/UIControls.hpp"
#include "Engine/Input/Input.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/TexDescSerializer.hpp"
#include "Engine/Asset/TextureAsset.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/FontAtlas.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/ITexture.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/RenderLayer.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Math/Matrix4.hpp"
#include "Math/Vector4.hpp"
#include "Math/Ray.hpp"
#include "Math/Plane.hpp"
#include "Engine/Renderer/SamplerMode.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Util/Utf8.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace fbzz::scene {

namespace {

struct UIVertex {
    math::Vector2 pos;
    math::Vector2 uv;
};

struct UIConstants {
    math::Matrix4 ortho;
    math::Vector4 color;
    math::Vector4 uvRect;
};

struct CanvasEntry {
    GameObject* go = nullptr;
    UICanvas* canvas = nullptr;
};

struct CanvasRuntimeState {
    math::Matrix4 canvasToClip = math::Matrix4::Identity();
    math::Vector2 mouseInCanvasSpace = {};
    renderer::ResourceHandle<renderer::PipelineStateTag> pso;
    renderer::RenderLayer layer = renderer::RenderLayer::OVERLAY_LAYER;
};

// Fixed VB capacities
static constexpr uint32_t kImageVBVertices = 6;
static constexpr uint32_t kTextVBVertices  = 4096; // ~682 グリフ分。超過時は複数ドローに分割する

struct Rect { math::Vector2 pos; math::Vector2 size; };

int GetUISortOrder(GameObject& go)
{
    // UIImage と UIText を同じ GO に持つ Button は一つの描画単位として扱う。
    // 両方に値がある場合は手前側を採用し、どちらの Inspector からでも調整できるようにする。
    int order = 0;
    if (const auto* image = go.GetComponent<UIImage>()) order = image->sortOrder;
    if (const auto* text = go.GetComponent<UIText>()) order = (std::max)(order, text->sortOrder);
    return order;
}

std::vector<GameObject*> SortedUIChildren(GameObject& go)
{
    std::vector<GameObject*> children;
    children.reserve(static_cast<size_t>(go.GetChildCount()));
    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i)) children.push_back(child);
    }
    // 同値時は Hierarchy 順を維持し、既存シーンの見た目を変えない。
    std::stable_sort(children.begin(), children.end(),
        [](GameObject* a, GameObject* b) {
            return GetUISortOrder(*a) < GetUISortOrder(*b);
        });
    return children;
}

struct UITransform2D {
    math::Vector2 position = math::Vector2::ZERO;
    float rotationZ = 0.0f;
};

float ExtractZRotation(const math::Quaternion& q)
{
    return std::atan2f(2.0f * (q.w * q.z + q.x * q.y),
                       1.0f - 2.0f * (q.y * q.y + q.z * q.z));
}

math::Vector2 Rotate2D(const math::Vector2& v, float angle)
{
    const float c = std::cosf(angle);
    const float s = std::sinf(angle);
    return { v.x * c - v.y * s, v.x * s + v.y * c };
}

UITransform2D ComposeUITransform(const UITransform2D& parent, const scene::Transform& local)
{
    // WHY: UI の localScale.xy は「倍率」ではなく「幅・高さ」なので、
    //      通常の TransformSystem のように親 scale を子 position へ掛けると、
    //      親要素のサイズ変更だけで子が大きく飛んでしまう。
    // WHAT: 親の位置と回転は継承し、サイズは各 UI 要素自身の localScale.xy から読む。
    const math::Vector2 localPos = { local.position.x, local.position.y };
    UITransform2D result{};
    result.position = parent.position + Rotate2D(localPos, parent.rotationZ);
    result.rotationZ = parent.rotationZ + ExtractZRotation(local.rotation);
    return result;
}

Rect RectFromTransform(const scene::Transform& t, const UITransform2D& resolved)
{
    return { resolved.position, { t.scale.x, t.scale.y } };
}

// ── Matrix4 × Vector4 ────────────────────────────────────────────────────────
// WHY: Matrix4 に operator*(Vector4) がない場合も UISystem 内で使えるよう
//      インライン実装する（WorldSpace ヒット判定で使用）。
static math::Vector4 MulMV(const math::Matrix4& m, const math::Vector4& v)
{
    return {
        m.m[0][0]*v.x + m.m[0][1]*v.y + m.m[0][2]*v.z + m.m[0][3]*v.w,
        m.m[1][0]*v.x + m.m[1][1]*v.y + m.m[1][2]*v.z + m.m[1][3]*v.w,
        m.m[2][0]*v.x + m.m[2][1]*v.y + m.m[2][2]*v.z + m.m[2][3]*v.w,
        m.m[3][0]*v.x + m.m[3][1]*v.y + m.m[3][2]*v.z + m.m[3][3]*v.w,
    };
}

// ── UISystemContext 初期化 ────────────────────────────────────────────────────
void EnsureInit(UISystemContext& ctx, renderer::ResourceManager& resources)
{
    if (ctx.initialized) return;
    ctx.initialized = true;

    ctx.shader     = resources.LoadShader("Assets/shaders/UI/UISprite.hlsl");
    ctx.textShader = resources.LoadShader("Assets/shaders/UI/UIText.hlsl");
    ctx.constants  = resources.CreateConstantBuffer(sizeof(UIConstants));
    ctx.pso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    // WorldSpace / ScreenSpaceCamera UI: 深度テストあり・深度書き込みなし
    ctx.worldPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });

    static constexpr uint8_t kWhite[4] = { 255, 255, 255, 255 };
    ctx.whiteTexture = resources.CreateTexture(kWhite, 1, 1);

    if (!ctx.shader.IsValid() || !ctx.textShader.IsValid() || !ctx.constants.IsValid() ||
        !ctx.pso.IsValid() || !ctx.worldPso.IsValid() || !ctx.whiteTexture.IsValid()) {
        FBZZ_LOG_ERROR("UISystem init failed: shader=%d textShader=%d cb=%d pso=%d worldPso=%d white=%d",
                       static_cast<int>(ctx.shader.IsValid()),
                       static_cast<int>(ctx.textShader.IsValid()),
                       static_cast<int>(ctx.constants.IsValid()),
                       static_cast<int>(ctx.pso.IsValid()),
                       static_cast<int>(ctx.worldPso.IsValid()),
                       static_cast<int>(ctx.whiteTexture.IsValid()));
    }
}

// この DrawCall 専用の頂点バッファを 1 本借りる。足りなければ増やし、以降は使い回す。
// WHY 借用制にするか: UISystemContext のコメントを参照。記録型バックエンドでは
//     「Draw ごとに別実体」が正しさの条件になる。
renderer::ResourceHandle<renderer::BufferTag> AcquireVertexBuffer(
    std::vector<renderer::ResourceHandle<renderer::BufferTag>>& pool,
    std::size_t& cursor,
    uint32_t vertexCapacity,
    renderer::ResourceManager& resources)
{
    if (cursor >= pool.size()) {
        pool.push_back(resources.CreateVertexBuffer(
            nullptr, vertexCapacity * sizeof(UIVertex), sizeof(UIVertex)));
    }
    return pool[cursor++];
}

// ── Canvas 収集 ──────────────────────────────────────────────────────────────
void CollectCanvasesRecursive(GameObject* go, std::vector<CanvasEntry>& canvases)
{
    if (!go || !go->activeInHierarchy()) return;
    if (auto* canvas = go->GetComponent<UICanvas>(); canvas && canvas->enabled)
        canvases.push_back({ go, canvas });
    for (int i = 0; i < go->GetChildCount(); ++i)
        CollectCanvasesRecursive(go->GetChild(i), canvases);
}

void CollectCanvases(Scene& scene, std::vector<CanvasEntry>& canvases)
{
    for (GameObject* root : scene.GetRootGameObjects()) {
        if (!root) continue;
        CollectCanvasesRecursive(root, canvases);
    }
    std::sort(canvases.begin(), canvases.end(),
        [](const CanvasEntry& a, const CanvasEntry& b) {
            return a.canvas->sortOrder < b.canvas->sortOrder;
        });
}

bool IsScreenSpaceRenderMode(UIRenderMode mode);

bool ShouldRenderCanvas(const UICanvas& canvas, UIRenderTargetView targetView)
{
    // WHY: GameViewport は最終出力として全 Canvas、SceneViewport は WorldSpace、
    //       CanvasEditor は ScreenSpace だけ描く。
    if (targetView == UIRenderTargetView::SceneViewport)
        return canvas.renderMode == UIRenderMode::WorldSpace;
    if (targetView == UIRenderTargetView::CanvasEditor)
        return IsScreenSpaceRenderMode(canvas.renderMode);
    return true;
}

bool IsScreenSpaceRenderMode(UIRenderMode mode)
{
    return mode == UIRenderMode::ScreenSpaceOverlay
        || mode == UIRenderMode::ScreenSpaceCamera;
}

float ResolveCanvasScale(const UICanvas& canvas, float viewportWidth, float viewportHeight)
{
    // WHY: Unity の Canvas Scaler と同じ考え方。基準解像度と現在 Viewport の差を
    //      UI 座標変換に集約する。
    if (canvas.scaleMode != UICanvasScaleMode::ScaleWithScreenSize)
        return 1.0f;

    const float refW = (std::max)(1.0f, canvas.referenceWidth);
    const float refH = (std::max)(1.0f, canvas.referenceHeight);
    const float scaleW = (std::max)(1.0f, viewportWidth) / refW;
    const float scaleH = (std::max)(1.0f, viewportHeight) / refH;
    const float match = std::clamp(canvas.matchWidthOrHeight, 0.0f, 1.0f);
    return std::exp(std::log(scaleW) * (1.0f - match) + std::log(scaleH) * match);
}

void ResolveScreenSpaceCanvasArea(const UICanvas& canvas,
                                  float viewportWidth,
                                  float viewportHeight,
                                  float& visibleCanvasW,
                                  float& visibleCanvasH)
{
    if (canvas.scaleMode == UICanvasScaleMode::ScaleWithScreenSize) {
        const float scale = ResolveCanvasScale(canvas, viewportWidth, viewportHeight);
        visibleCanvasW = (std::max)(1.0f, viewportWidth) / scale;
        visibleCanvasH = (std::max)(1.0f, viewportHeight) / scale;
        return;
    }
    visibleCanvasW = (std::max)(1.0f, canvas.canvasWidth);
    visibleCanvasH = (std::max)(1.0f, canvas.canvasHeight);
}

CanvasRuntimeState BuildCanvasRuntimeState(const UICanvas& canvas,
                                           const GameObject& canvasGO,
                                           float viewportWidth,
                                           float viewportHeight,
                                           math::Vector2 rawMouseInViewport,
                                           const math::Matrix4& viewProjection,
                                           math::Vector3 cameraWorldPos,
                                           math::Quaternion cameraWorldRot,
                                           const UISystemContext& ctx,
                                           UIRenderTargetView targetView)
{
    (void)targetView;
    CanvasRuntimeState state{};

    // ── WorldSpace ────────────────────────────────────────────────────────────
    if (canvas.renderMode == UIRenderMode::WorldSpace) {
        const float ws = canvas.worldScale;

        math::Matrix4 pixelToLocal = math::Matrix4::Identity();
        pixelToLocal.m[0][0] =  ws;
        pixelToLocal.m[1][1] = -ws;
        pixelToLocal.m[0][3] = -canvas.canvasWidth  * 0.5f * ws;
        pixelToLocal.m[1][3] =  canvas.canvasHeight * 0.5f * ws;

        // WHY: 再帰収集後は Canvas が子 GO に置かれる可能性があるため worldPosition を使う。
        //      faceCamera 時は向きをカメラ姿勢で上書きし、常にカメラへ正対させる (ビルボード)。
        //      位置は worldPosition のままなので、敵の頭上に置けば追従しつつ常に読める。
        const math::Quaternion canvasRotation =
            canvas.faceCamera ? cameraWorldRot : canvasGO.transform.worldRotation;
        const math::Matrix4 worldMatrix = math::Matrix4::TRS(
            canvasGO.transform.worldPosition,
            canvasRotation,
            math::Vector3::ONE
        );

        state.canvasToClip = viewProjection * worldMatrix * pixelToLocal;
        state.mouseInCanvasSpace = rawMouseInViewport; // WorldSpace はレイキャストで処理
        state.pso   = ctx.worldPso;
        state.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
        return state;
    }

    // ── ScreenSpaceCamera ─────────────────────────────────────────────────────
    if (canvas.renderMode == UIRenderMode::ScreenSpaceCamera) {
        // Canvas をカメラ前方 planeDistance ワールド単位に配置し、カメラ向きで固定する。
        // WHY: Overlay と同じスクリーン UI に深度テストを加えたい場合に使う。
        //      マウス座標変換は Overlay と同じスクリーン座標系を維持する。
        const float ws = canvas.worldScale;

        math::Matrix4 pixelToLocal = math::Matrix4::Identity();
        pixelToLocal.m[0][0] =  ws;
        pixelToLocal.m[1][1] = -ws;
        pixelToLocal.m[0][3] = -canvas.canvasWidth  * 0.5f * ws;
        pixelToLocal.m[1][3] =  canvas.canvasHeight * 0.5f * ws;

        // カメラ前方ベクトル: worldRot * (0, 0, 1)
        const math::Vector3 camFwd = cameraWorldRot * math::Vector3{ 0.0f, 0.0f, 1.0f };
        const math::Vector3 canvasPos = cameraWorldPos + camFwd * canvas.planeDistance;
        const math::Matrix4 worldMatrix = math::Matrix4::TRS(
            canvasPos, cameraWorldRot, math::Vector3::ONE);

        state.canvasToClip = viewProjection * worldMatrix * pixelToLocal;
        // マウス座標: Overlay と同じスクリーン→キャンバス変換
        float visibleW = 1.0f, visibleH = 1.0f;
        ResolveScreenSpaceCanvasArea(canvas, viewportWidth, viewportHeight, visibleW, visibleH);
        const float scaleX = (std::max)(1.0f, viewportWidth) / visibleW;
        const float scaleY = (std::max)(1.0f, viewportHeight) / visibleH;
        state.mouseInCanvasSpace = { rawMouseInViewport.x / scaleX, rawMouseInViewport.y / scaleY };
        state.pso   = ctx.worldPso;  // 深度テストあり (3D オブジェクトに遮蔽可)
        state.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
        return state;
    }

    // ── ScreenSpaceOverlay (デフォルト) ───────────────────────────────────────
    float visibleCanvasW = 1.0f;
    float visibleCanvasH = 1.0f;
    ResolveScreenSpaceCanvasArea(canvas, viewportWidth, viewportHeight,
                                 visibleCanvasW, visibleCanvasH);

    const float scaleX = (std::max)(1.0f, viewportWidth) / visibleCanvasW;
    const float scaleY = (std::max)(1.0f, viewportHeight) / visibleCanvasH;

    state.canvasToClip = math::Matrix4::Orthographic(
        0.0f, visibleCanvasW,
        visibleCanvasH, 0.0f,
        0.0f, 1.0f);
    state.mouseInCanvasSpace = { rawMouseInViewport.x / scaleX, rawMouseInViewport.y / scaleY };
    state.pso   = ctx.pso;
    state.layer = renderer::RenderLayer::OVERLAY_LAYER;
    return state;
}

// ── UIButton 更新 ─────────────────────────────────────────────────────────────
bool UpdateButton(UIButton& button, const Rect& rect, math::Vector2 mouse, bool mousePressed)
{
    const UIButtonState previousState = button.state;
    button.onClick = false;
    button.onEnter = false;
    button.onExit  = false;

    if (!button.enabled || !button.isInteractable) {
        button.state            = UIButtonState::NORMAL;
        button.wasPressedOnThis = false;
        button.lastMouseState   = mousePressed;
        return false;
    }

    const bool hit               = mouse.x >= rect.pos.x && mouse.x <= rect.pos.x + rect.size.x
                                && mouse.y >= rect.pos.y && mouse.y <= rect.pos.y + rect.size.y;
    const bool mouseJustPressed  = mousePressed  && !button.lastMouseState;
    const bool mouseJustReleased = !mousePressed && button.lastMouseState;

    // WHY: ドラッグで流入した押下をクリックとして誤検出しないため。
    if (mouseJustPressed && hit)
        button.wasPressedOnThis = true;

    if (mouseJustReleased) {
        button.onClick          = button.wasPressedOnThis && hit;
        button.wasPressedOnThis = false;
    }

    button.lastMouseState = mousePressed;

    if (hit && mousePressed && button.wasPressedOnThis) {
        button.state = UIButtonState::PRESSED;
    } else if (hit) {
        button.state = UIButtonState::HOVERED;
    } else {
        button.state = UIButtonState::NORMAL;
    }

    button.onEnter = previousState == UIButtonState::NORMAL && button.state != UIButtonState::NORMAL;
    button.onExit  = previousState != UIButtonState::NORMAL && button.state == UIButtonState::NORMAL;
    return hit;
}

math::Vector4 ButtonTint(const UIButton& button)
{
    if (!button.enabled || !button.isInteractable) return button.disabledColor;
    if (button.state == UIButtonState::PRESSED) return button.pressedColor;
    if (button.state == UIButtonState::HOVERED) return button.hoverColor;
    return button.normalColor;
}

math::Vector4 ResolveButtonImageColor(const math::Vector4& imageColor, const UIButton& button)
{
    const math::Vector4 targetColor = ButtonTint(button);
    constexpr float MIN_COLOR_CHANNEL = 1.0e-5f;
    const auto relativeChannel = [=](float image, float normal, float target) {
        return normal > MIN_COLOR_CHANNEL ? image * (target / normal) : target;
    };

    // WHY: 単純な乗算ではUIImageとUIButtonの両方が暗色の場合にPressed Colorが黒へ潰れる。
    //      Normal Colorを基準に相対変換すれば、UIImage.color == normalColorの一般的な設定で
    //      Inspectorに指定したHover/Pressed Colorがそのまま画面へ反映される。
    return {
        relativeChannel(imageColor.x, button.normalColor.x, targetColor.x),
        relativeChannel(imageColor.y, button.normalColor.y, targetColor.y),
        relativeChannel(imageColor.z, button.normalColor.z, targetColor.z),
        relativeChannel(imageColor.w, button.normalColor.w, targetColor.w)
    };
}

// ── 描画サブミット ────────────────────────────────────────────────────────────
void SubmitImage(renderer::IRenderer& renderer,
                 renderer::ResourceManager& resources,
                 UISystemContext& ctx,
                 const math::Matrix4& canvasToClip,
                 renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                 renderer::RenderLayer layer,
                 const math::Vector2& position,
                 const math::Vector2& size,
                 const math::Vector4& color,
                 const math::Vector2& uvMin,
                 const math::Vector2& uvMax,
                 renderer::ResourceHandle<renderer::TextureTag> texture,
                 float zAngle = 0.0f)
{
    const float cx = position.x + size.x * 0.5f;
    const float cy = position.y + size.y * 0.5f;
    const float cosZ = std::cosf(zAngle);
    const float sinZ = std::sinf(zAngle);
    const float hW = size.x * 0.5f, hH = size.y * 0.5f;

    auto rot = [&](float lx, float ly) -> math::Vector2 {
        return { cx + lx * cosZ - ly * sinZ,
                 cy + lx * sinZ + ly * cosZ };
    };

    UIVertex vertices[6] = {
        { rot(-hW, -hH), { uvMin.x, uvMin.y } },
        { rot(-hW,  hH), { uvMin.x, uvMax.y } },
        { rot( hW, -hH), { uvMax.x, uvMin.y } },
        { rot( hW, -hH), { uvMax.x, uvMin.y } },
        { rot(-hW,  hH), { uvMin.x, uvMax.y } },
        { rot( hW,  hH), { uvMax.x, uvMax.y } },
    };
    const auto vertexBuffer = AcquireVertexBuffer(
        ctx.imageVertexBuffers, ctx.imageVertexCursor, kImageVBVertices, resources);
    if (!vertexBuffer.IsValid()) return;
    resources.Update(vertexBuffer, vertices, sizeof(vertices));

    UIConstants constants{};
    constants.ortho  = canvasToClip;
    constants.color  = color;
    constants.uvRect = { uvMin.x, uvMin.y, uvMax.x, uvMax.y };
    resources.Update(ctx.constants, &constants, sizeof(constants));

    renderer::DrawCall call;
    call.vertexBuffer       = vertexBuffer;
    call.shader             = ctx.shader;
    call.pipelineState      = pso;
    call.constantBuffers[0] = ctx.constants;
    call.vertexCount        = 6;
    call.layer              = layer;
    call.topology           = renderer::PrimitiveTopology::TRIANGLE_LIST;
    call.textures[0]        = texture.IsValid() ? texture : ctx.whiteTexture;
    renderer.Submit(call, resources);
}

// Sprite Borderを保った9-slice描画。四隅は固定サイズ、辺は一方向、中央だけを両方向へ伸縮する。
void SubmitSlicedImage(renderer::IRenderer& renderer,
                       renderer::ResourceManager& resources,
                       UISystemContext& ctx,
                       const math::Matrix4& canvasToClip,
                       renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                       renderer::RenderLayer layer,
                       const math::Vector2& position,
                       const math::Vector2& size,
                       const math::Vector4& color,
                       const math::Vector2& uvMin,
                       const math::Vector2& uvMax,
                       const math::Vector4& border,
                       const math::Vector2& textureSize,
                       renderer::ResourceHandle<renderer::TextureTag> texture,
                       float zAngle)
{
    if (textureSize.x <= 0.0f || textureSize.y <= 0.0f
        || (border.x <= 0.0f && border.y <= 0.0f
            && border.z <= 0.0f && border.w <= 0.0f)) {
        SubmitImage(renderer, resources, ctx, canvasToClip, pso, layer,
                    position, size, color, uvMin, uvMax, texture, zAngle);
        return;
    }

    const float sourceWidth = (uvMax.x - uvMin.x) * textureSize.x;
    const float sourceHeight = (uvMax.y - uvMin.y) * textureSize.y;
    const float sourceLeft = std::min(border.x, sourceWidth * 0.5f);
    const float sourceTop = std::min(border.y, sourceHeight * 0.5f);
    const float sourceRight = std::min(border.z, sourceWidth - sourceLeft);
    const float sourceBottom = std::min(border.w, sourceHeight - sourceTop);
    const float left = std::min(sourceLeft, size.x * 0.5f);
    const float top = std::min(sourceTop, size.y * 0.5f);
    const float right = std::min(sourceRight, size.x - left);
    const float bottom = std::min(sourceBottom, size.y - top);
    const float x[4] = { 0.0f, left, size.x - right, size.x };
    const float y[4] = { 0.0f, top, size.y - bottom, size.y };
    const float u[4] = {
        uvMin.x,
        uvMin.x + sourceLeft / textureSize.x,
        uvMax.x - sourceRight / textureSize.x,
        uvMax.x
    };
    const float v[4] = {
        uvMin.y,
        uvMin.y + sourceTop / textureSize.y,
        uvMax.y - sourceBottom / textureSize.y,
        uvMax.y
    };

    const math::Vector2 fullCenter = {
        position.x + size.x * 0.5f,
        position.y + size.y * 0.5f
    };
    const float cosZ = std::cosf(zAngle);
    const float sinZ = std::sinf(zAngle);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const math::Vector2 cellSize = {
                x[column + 1] - x[column],
                y[row + 1] - y[row]
            };
            if (cellSize.x <= 0.0f || cellSize.y <= 0.0f) continue;
            const math::Vector2 localCenter = {
                x[column] + cellSize.x * 0.5f - size.x * 0.5f,
                y[row] + cellSize.y * 0.5f - size.y * 0.5f
            };
            const math::Vector2 rotatedCenter = {
                fullCenter.x + localCenter.x * cosZ - localCenter.y * sinZ,
                fullCenter.y + localCenter.x * sinZ + localCenter.y * cosZ
            };
            const math::Vector2 cellPosition = {
                rotatedCenter.x - cellSize.x * 0.5f,
                rotatedCenter.y - cellSize.y * 0.5f
            };
            SubmitImage(renderer, resources, ctx, canvasToClip, pso, layer,
                        cellPosition, cellSize, color,
                        { u[column], v[row] },
                        { u[column + 1], v[row + 1] },
                        texture, zAngle);
        }
    }
}

void SubmitRect(renderer::IRenderer& renderer,
                renderer::ResourceManager& resources,
                UISystemContext& ctx,
                const math::Matrix4& canvasToClip,
                renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                renderer::RenderLayer layer,
                math::Vector2 position,
                math::Vector2 size,
                const math::Vector4& color)
{
    SubmitImage(renderer, resources, ctx, canvasToClip, pso, layer,
                position, size, color,
                { 0.0f, 0.0f }, { 1.0f, 1.0f }, ctx.whiteTexture);
}

// ── フォントアトラス ──────────────────────────────────────────────────────────
renderer::FontAtlas& GetOrLoadFontAtlas(const std::string& basePath,
                                        UISystemContext& ctx,
                                        renderer::ResourceManager& resources)
{
    auto it = ctx.fontAtlasCache.find(basePath);
    if (it != ctx.fontAtlasCache.end())
        return it->second;

    renderer::FontAtlas& atlas = ctx.fontAtlasCache[basePath];
    if (!atlas.Load(basePath, resources))
        FBZZ_LOG_ERROR("UISystem: failed to load FontAtlas: %s", basePath.c_str());

    return atlas;
}

// テキストを UTF-8 コードポイント単位で走査し、行ごとの表示幅 (スケール適用済み) を返す。
// 改行で区切られた行数だけ要素が入る (空文字列でも 1 要素)。
//
// WHY: レイアウト計算 (ComputeTextLogicalSize) と描画 (SubmitTextWithAtlas) の両方が
//      まったく同じ送り幅の規則を必要とする。ここに 1 本化しないと、
//      カーニングや UTF-8 の扱いが片方だけズレて中央揃えが崩れる。
std::vector<float> ComputeLineWidths(const UIText& text,
                                     const renderer::FontAtlas& atlas,
                                     float scale)
{
    std::vector<float> lineWidths;
    float    lineW    = 0.0f;
    char32_t previous = 0;

    std::size_t offset = 0;
    while (offset < text.text.size()) {
        const char32_t code = util::Utf8::Decode(text.text, offset);
        if (code == U'\n') {
            lineWidths.push_back(lineW);
            lineW    = 0.0f;
            previous = 0;
            continue;
        }

        // カーニングは「前の文字との組」に対して定義されるため、行頭では適用しない。
        if (previous != 0)
            lineW += atlas.GetKerning(previous, code) * scale;

        const renderer::FontGlyph* glyph = atlas.GetGlyph(code);
        const float advance = glyph ? glyph->advance : atlas.GetFallbackAdvance();
        lineW += advance * scale + text.letterSpacing;
        previous = code;
    }
    lineWidths.push_back(lineW);
    return lineWidths;
}

math::Vector2 ComputeTextLogicalSize(const UIText& text,
                                     UISystemContext& ctx,
                                     renderer::ResourceManager& resources)
{
    const std::string& path = text.fontPath.empty() ? ctx.defaultFontPath : text.fontPath;
    renderer::FontAtlas& atlas = GetOrLoadFontAtlas(path, ctx, resources);
    if (!atlas.IsValid()) return {};

    // 動的フォントでは、この文字列に必要なグリフをここで焼く。
    // WHY: レイアウトは送り幅を必要とするため、幅を測る前に登録が済んでいる必要がある。
    //      静的フォントでは即 return するのでコストはかからない。
    atlas.PrepareText(text.text, resources);

    const float scale = text.fontSize / atlas.GetLineHeight();
    const float cellH = atlas.GetLineHeight() * scale;

    const std::vector<float> lineWidths = ComputeLineWidths(text, atlas, scale);

    float maxW = 0.0f;
    for (float lineW : lineWidths)
        maxW = (std::max)(maxW, lineW);

    return { maxW, cellH * static_cast<float>(lineWidths.size()) };
}

void UpdateTextSizesRecursive(GameObject& go,
                              UISystemContext& ctx,
                              renderer::ResourceManager& resources)
{
    if (!go.activeInHierarchy()) return;
    if (auto* text = go.GetComponent<UIText>(); text && text->enabled && !text->text.empty()) {
        const math::Vector2 size = ComputeTextLogicalSize(*text, ctx, resources);
        go.transform.scale.x = size.x;
        go.transform.scale.y = size.y;
    }
    for (int i = 0; i < go.GetChildCount(); ++i)
        if (GameObject* child = go.GetChild(i))
            UpdateTextSizesRecursive(*child, ctx, resources);
}

void UITextSizeSystem(const std::vector<CanvasEntry>& canvases,
                      UISystemContext& ctx,
                      renderer::ResourceManager& resources)
{
    for (const CanvasEntry& entry : canvases)
        UpdateTextSizesRecursive(*entry.go, ctx, resources);
}

// ── テキスト描画（マルチドロー対応）─────────────────────────────────────────
void SubmitTextWithAtlas(renderer::IRenderer& renderer,
                         renderer::ResourceManager& resources,
                         UISystemContext& ctx,
                         const math::Matrix4& canvasToClip,
                         renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                         renderer::RenderLayer layer,
                         const UIText& text,
                         math::Vector2 position)
{
    renderer::FontAtlas& atlas = GetOrLoadFontAtlas(text.fontPath, ctx, resources);
    if (!atlas.IsValid()) return;

    // 描画経路からも焼いておく。
    // WHY: UITextSizeSystem を通らずに描画されるテキスト (サイズ自動調整の対象外) でも
    //      グリフが揃っている必要がある。登録済みなら UTF-8 走査だけで戻る。
    atlas.PrepareText(text.text, resources);

    const float scale = text.fontSize / atlas.GetLineHeight();
    const float cellH = atlas.GetLineHeight() * scale;

    // 整列と改行位置の決定に行幅が必要なので、常に事前計算する。
    // WHY: 旧実装は Left 以外のときだけ計算していたが、UTF-8 走査を 2 度書く方が
    //      ズレの温床になる。1 行あたり数十文字の走査であり、コストは無視できる。
    const std::vector<float> lineWidths = ComputeLineWidths(text, atlas, scale);

    int lineIdx = 0;
    auto lineStartX = [&]() -> float {
        if (text.align == TextAlign::Left) return position.x;
        const float lw = lineIdx < static_cast<int>(lineWidths.size()) ? lineWidths[lineIdx] : 0.0f;
        if (text.align == TextAlign::Center) return position.x - lw * 0.5f;
        return position.x - lw; // Right
    };

    math::Vector2 pen = { lineStartX(), position.y };

    // ページごとに頂点を分ける。
    // WHY: BMFont のマルチページアトラス (日本語のように 1 枚へ収まらない字種) では
    //      グリフごとに参照テクスチャが変わる。ページ単位でまとめてから
    //      ドローコールを分けることで、テクスチャ切り替えを最小回数に抑える。
    std::vector<std::vector<UIVertex>> pageVerts(atlas.GetPageCount());
    if (pageVerts.empty()) return;
    pageVerts[0].reserve(text.text.size() * 6);

    char32_t    previous = 0;
    std::size_t cursor   = 0;
    while (cursor < text.text.size()) {
        const char32_t code = util::Utf8::Decode(text.text, cursor);
        if (code == U'\n') {
            ++lineIdx;
            pen.x    = lineStartX();
            pen.y   += cellH;
            previous = 0;
            continue;
        }

        if (previous != 0)
            pen.x += atlas.GetKerning(previous, code) * scale;
        previous = code;

        const renderer::FontGlyph* g = atlas.GetGlyph(code);
        if (!g) {
            pen.x += atlas.GetFallbackAdvance() * scale + text.letterSpacing;
            continue;
        }

        // 空白文字のように画像を持たないグリフは、送りだけ進めて四角形を出さない。
        if (g->width > 0.0f && g->height > 0.0f) {
            const float x  = pen.x + g->xOffset * scale;
            const float y  = pen.y + g->yOffset * scale;
            const float x2 = x + g->width  * scale;
            const float y2 = y + g->height * scale;

            const std::size_t page =
                (g->page >= 0 && static_cast<std::size_t>(g->page) < pageVerts.size())
                    ? static_cast<std::size_t>(g->page) : 0;
            std::vector<UIVertex>& verts = pageVerts[page];

            verts.push_back({ {x,  y},  { g->u0, g->v0 } });
            verts.push_back({ {x,  y2}, { g->u0, g->v1 } });
            verts.push_back({ {x2, y},  { g->u1, g->v0 } });
            verts.push_back({ {x2, y},  { g->u1, g->v0 } });
            verts.push_back({ {x,  y2}, { g->u0, g->v1 } });
            verts.push_back({ {x2, y2}, { g->u1, g->v1 } });
        }

        pen.x += g->advance * scale + text.letterSpacing;
    }

    // 定数バッファは全ページ・全チャンク共通なので 1 回だけ更新する。
    UIConstants constants{};
    constants.ortho  = canvasToClip;
    constants.color  = text.color;
    constants.uvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
    resources.Update(ctx.constants, &constants, sizeof(constants));

    renderer::DrawCall call;
    // WHY: UIText.hlsl の .r チャンネルを coverage として使い、alpha チャンネル依存を排除する。
    call.shader             = ctx.textShader;
    call.pipelineState      = pso;
    call.constantBuffers[0] = ctx.constants;
    call.layer              = layer;
    call.topology           = renderer::PrimitiveTopology::TRIANGLE_LIST;

    for (std::size_t page = 0; page < pageVerts.size(); ++page) {
        const std::vector<UIVertex>& verts = pageVerts[page];
        if (verts.empty()) continue;

        call.textures[0] = atlas.GetTexture(static_cast<int>(page));
        if (!call.textures[0].IsValid()) continue;

        // kTextVBVertices を超えるテキストはチャンク分割して複数ドローコールで描く。
        // WHY: 固定 VB サイズを超えてもエラーで打ち切らず全グリフを描画するため。
        uint32_t offset = 0;
        const uint32_t total = static_cast<uint32_t>(verts.size());
        while (offset < total) {
            const uint32_t chunk = (std::min)(total - offset, kTextVBVertices);
            // チャンクごとに別実体を借りる。1 本を上書きすると、記録型バックエンドでは
            // 全チャンクが最後のグリフ群になる。
            const auto vertexBuffer = AcquireVertexBuffer(
                ctx.textVertexBuffers, ctx.textVertexCursor, kTextVBVertices, resources);
            if (!vertexBuffer.IsValid()) break;
            resources.Update(vertexBuffer, verts.data() + offset, chunk * sizeof(UIVertex));
            call.vertexBuffer = vertexBuffer;
            call.vertexCount  = chunk;
            renderer.Submit(call, resources);
            offset += chunk;
        }
    }
}

void SubmitText(renderer::IRenderer& renderer,
                renderer::ResourceManager& resources,
                UISystemContext& ctx,
                const math::Matrix4& canvasToClip,
                renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                renderer::RenderLayer layer,
                const UIText& text,
                math::Vector2 position)
{
    if (!text.enabled || text.text.empty() || text.fontSize <= 0.0f) return;

    if (text.fontPath.empty()) {
        UIText defaulted = text;
        defaulted.fontPath = ctx.defaultFontPath;
        SubmitTextWithAtlas(renderer, resources, ctx, canvasToClip, pso, layer, defaulted, position);
        return;
    }
    SubmitTextWithAtlas(renderer, resources, ctx, canvasToClip, pso, layer, text, position);
}

// ── UILayoutGroup ─────────────────────────────────────────────────────────────
void ApplyLayout(GameObject& go, const UILayoutGroup& layout, float canvasScale)
{
    static std::unordered_set<const UILayoutGroup*> s_warnedLayouts;

    const int count = go.GetChildCount();

    std::vector<int> indices;
    indices.reserve(count);
    for (int i = 0; i < count; ++i) {
        GameObject* child = go.GetChild(i);
        if (!child || !child->activeInHierarchy()) continue;
        indices.push_back(i);
    }
    if (layout.reverseOrder)
        std::reverse(indices.begin(), indices.end());

    float cursor = (layout.axis == UILayoutAxis::Horizontal) ? layout.paddingLeft : layout.paddingTop;
    for (int idx : indices) {
        GameObject* child = go.GetChild(idx);
        auto& t = child->transform;
        if (layout.axis == UILayoutAxis::Horizontal) {
            t.position.x = cursor;
            t.position.y = layout.paddingTop;
            cursor += t.scale.x + layout.spacing;
        } else {
            t.position.x = layout.paddingLeft;
            t.position.y = cursor;
            cursor += t.scale.y + layout.spacing;
        }
    }

    // paddingRight / paddingBottom がコンテナ末端として機能しているか検証する。
    // WHY: レイアウトはクリッピングを行わないためはみ出しは描画バグとして現れる。
    if (!indices.empty()) {
        const float endPos  = cursor - layout.spacing;
        const float contW   = go.transform.scale.x;
        const float contH   = go.transform.scale.y;
        const bool overflow = (layout.axis == UILayoutAxis::Horizontal)
            ? (endPos > contW - layout.paddingRight)
            : (endPos > contH - layout.paddingBottom);

        if (overflow && s_warnedLayouts.find(&layout) == s_warnedLayouts.end()) {
            s_warnedLayouts.insert(&layout);
            const float excess = (layout.axis == UILayoutAxis::Horizontal)
                ? (endPos - (contW - layout.paddingRight))
                : (endPos - (contH - layout.paddingBottom));
            FBZZ_LOG_WARN("UILayoutGroup: children overflow container by %.1f canvas-px (%.1f viewport-px, canvasScale=%.2f)",
                          excess, excess * canvasScale, canvasScale);
        }
    }
}

void ApplyUILayoutRecursive(GameObject& go, float canvasScale)
{
    if (!go.activeInHierarchy()) return;

    if (auto* layout = go.GetComponent<UILayoutGroup>(); layout && layout->enabled)
        ApplyLayout(go, *layout, canvasScale);

    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i))
            ApplyUILayoutRecursive(*child, canvasScale);
    }
}

// ── UIButton イベント処理 ─────────────────────────────────────────────────────
bool ProcessUIEventsRecursive(GameObject& go,
                              const UITransform2D& parentTransform,
                              math::Vector2 mouseInCanvasSpace,
                              bool mousePressed,
                              bool inputAvailable = true,
                              bool applySelfTransform = true)
{
    if (!go.activeInHierarchy()) return false;

    const UITransform2D resolved = applySelfTransform
        ? ComposeUITransform(parentTransform, go.transform)
        : parentTransform;

    const Rect selfRect = RectFromTransform(go.transform, resolved);
    const bool insideSelf = mouseInCanvasSpace.x >= selfRect.pos.x
        && mouseInCanvasSpace.x <= selfRect.pos.x + selfRect.size.x
        && mouseInCanvasSpace.y >= selfRect.pos.y
        && mouseInCanvasSpace.y <= selfRect.pos.y + selfRect.size.y;
    bool childrenInputAvailable = inputAvailable;
    if (const auto* mask = go.GetComponent<UIMask>();
        mask && mask->enabled && mask->affectChildren && !insideSelf)
        childrenInputAvailable = false;
    UITransform2D childTransform = resolved;
    if (const auto* scroll = go.GetComponent<UIScrollView>(); scroll && scroll->enabled)
        childTransform.position -= Rotate2D(scroll->scrollPosition, resolved.rotationZ);

    // 描画順の逆から入力を解決し、重なった UI では最前面の要素だけがポインターを受け取る。
    bool consumed = false;
    auto children = SortedUIChildren(go);
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        consumed |= ProcessUIEventsRecursive(**it, childTransform, mouseInCanvasSpace, mousePressed,
                                             childrenInputAvailable && !consumed);
    }

    auto* button = go.GetComponent<UIButton>();
    if (button) {
        // UIImage の有無に関わらず transform.scale.xy が有効ならヒット判定する。
        // WHY: UIImage なしで UIText / 子要素だけで構成されるボタンにも対応する。
        if (go.transform.scale.x > 0.0f && go.transform.scale.y > 0.0f) {
            const math::Vector2 effectiveMouse = inputAvailable && !consumed
                ? mouseInCanvasSpace
                : math::Vector2{ -1.0e30f, -1.0e30f };
            consumed |= UpdateButton(*button, RectFromTransform(go.transform, resolved),
                                     effectiveMouse, mousePressed);
        }
        else {
            button->onClick          = false;
            button->onEnter          = false;
            button->onExit           = false;
            button->state            = UIButtonState::NORMAL;
            button->wasPressedOnThis = false;
            button->lastMouseState   = mousePressed;
        }
    }

    const Rect rect = RectFromTransform(go.transform, resolved);
    const bool hit = mouseInCanvasSpace.x >= rect.pos.x
        && mouseInCanvasSpace.x <= rect.pos.x + rect.size.x
        && mouseInCanvasSpace.y >= rect.pos.y
        && mouseInCanvasSpace.y <= rect.pos.y + rect.size.y;
    const bool canReceive = inputAvailable && !consumed;

    if (auto* slider = go.GetComponent<UISlider>()) {
        slider->onValueChanged = false;
        const bool justPressed = mousePressed && !slider->runtimeLastMouse;
        if (slider->enabled && slider->interactable && canReceive && justPressed && hit)
            slider->runtimeDragging = true;
        if (!mousePressed)
            slider->runtimeDragging = false;
        if (slider->runtimeDragging) {
            const float axisSize = slider->vertical ? rect.size.y : rect.size.x;
            const float axisPosition = slider->vertical
                ? mouseInCanvasSpace.y - rect.pos.y
                : mouseInCanvasSpace.x - rect.pos.x;
            float normalized = axisSize > 0.0f
                ? std::clamp(axisPosition / axisSize, 0.0f, 1.0f) : 0.0f;
            if (slider->vertical)
                normalized = 1.0f - normalized;
            float next = slider->minimum + (slider->maximum - slider->minimum) * normalized;
            if (slider->wholeNumbers)
                next = std::round(next);
            next = std::clamp(next, (std::min)(slider->minimum, slider->maximum),
                              (std::max)(slider->minimum, slider->maximum));
            slider->onValueChanged = next != slider->value;
            slider->value = next;
            if (auto* sliderImage = go.GetComponent<UIImage>()) {
                const float range = slider->maximum - slider->minimum;
                sliderImage->fillAmount = std::abs(range) > 0.000001f
                    ? std::clamp((slider->value - slider->minimum) / range, 0.0f, 1.0f)
                    : 0.0f;
                sliderImage->fillOrigin = slider->vertical
                    ? UIImageFillOrigin::Bottom : UIImageFillOrigin::Left;
            }
            consumed = true;
        }
        slider->runtimeLastMouse = mousePressed;
    }

    if (auto* toggle = go.GetComponent<UIToggle>()) {
        toggle->onValueChanged = false;
        const bool justPressed = mousePressed && !toggle->runtimeLastMouse;
        const bool justReleased = !mousePressed && toggle->runtimeLastMouse;
        if (toggle->enabled && toggle->interactable && canReceive && justPressed && hit)
            toggle->runtimePressedHere = true;
        if (justReleased) {
            if (toggle->runtimePressedHere && hit) {
                toggle->isOn = !toggle->isOn;
                toggle->onValueChanged = true;
                consumed = true;
            }
            toggle->runtimePressedHere = false;
        }
        toggle->runtimeLastMouse = mousePressed;
    }

    if (auto* scroll = go.GetComponent<UIScrollView>()) {
        scroll->onValueChanged = false;
        const float wheel = input::Input::MouseScrollDelta();
        if (scroll->enabled && canReceive && hit && wheel != 0.0f) {
            if (scroll->vertical)
                scroll->scrollPosition.y -= wheel * scroll->sensitivity;
            if (scroll->horizontal)
                scroll->scrollPosition.x -= wheel * scroll->sensitivity;
            scroll->scrollPosition.x = std::clamp(scroll->scrollPosition.x, 0.0f,
                (std::max)(0.0f, scroll->contentSize.x - rect.size.x));
            scroll->scrollPosition.y = std::clamp(scroll->scrollPosition.y, 0.0f,
                (std::max)(0.0f, scroll->contentSize.y - rect.size.y));
            scroll->onValueChanged = true;
            consumed = true;
        }
    }

    if (auto* field = go.GetComponent<UIInputField>()) {
        field->onValueChanged = false;
        field->onSubmit = false;
        if (mousePressed && canReceive)
            field->focused = hit;
        if (field->enabled && field->interactable && field->focused) {
            for (const char character : input::Input::TextInput()) {
                if (character == '\b') {
                    if (!field->text.empty()) {
                        size_t characterStart = field->text.size() - 1;
                        while (characterStart > 0
                               && (static_cast<unsigned char>(field->text[characterStart]) & 0xC0u) == 0x80u)
                            --characterStart;
                        field->text.erase(characterStart);
                        field->onValueChanged = true;
                    }
                } else if (character == '\r' || character == '\n') {
                    if (field->multiline) {
                        field->text.push_back('\n');
                        field->onValueChanged = true;
                    } else {
                        field->onSubmit = true;
                        field->focused = false;
                    }
                } else if ((field->contentType == UIInputContentType::Standard
                            || field->contentType == UIInputContentType::Password
                            || (field->contentType == UIInputContentType::Integer
                                && (character >= '0' && character <= '9'
                                    || character == '-' && field->text.empty()))
                            || (field->contentType == UIInputContentType::Decimal
                                && (character >= '0' && character <= '9'
                                    || character == '-' && field->text.empty()
                                    || character == '.' && field->text.find('.') == std::string::npos)))
                           && (field->characterLimit <= 0
                               || static_cast<int>(field->text.size()) < field->characterLimit)) {
                    field->text.push_back(character);
                    field->onValueChanged = true;
                }
            }
            field->caretPosition = field->text.size();
            consumed |= hit;
        }
        if (auto* inputText = go.GetComponent<UIText>()) {
            if (field->text.empty()) {
                inputText->text = field->placeholder;
            } else if (field->contentType == UIInputContentType::Password) {
                inputText->text.assign(field->text.size(), '*');
            } else {
                inputText->text = field->text;
            }
        }
    }

    if (auto* trigger = go.GetComponent<UIEventTrigger>()) {
        trigger->pointerEnter = trigger->pointerExit = false;
        trigger->pointerDown = trigger->pointerUp = trigger->pointerClick = false;
        trigger->beginDrag = trigger->drag = trigger->endDrag = false;
        trigger->pointerPosition = mouseInCanvasSpace;
        trigger->dragDelta = mouseInCanvasSpace - trigger->runtimeLastPointer;
        const bool currentHit = trigger->enabled && canReceive && hit;
        trigger->pointerEnter = currentHit && !trigger->runtimeHovered;
        trigger->pointerExit = !currentHit && trigger->runtimeHovered;
        trigger->pointerDown = currentHit && mousePressed && !trigger->runtimeLastMouse;
        trigger->pointerUp = !mousePressed && trigger->runtimeLastMouse
            && trigger->runtimePressedHere;
        if (trigger->pointerDown) {
            trigger->runtimePressedHere = true;
            trigger->beginDrag = true;
        }
        trigger->drag = trigger->runtimePressedHere && mousePressed
            && trigger->dragDelta != math::Vector2::ZERO;
        if (trigger->pointerUp) {
            trigger->pointerClick = currentHit;
            trigger->endDrag = true;
            trigger->runtimePressedHere = false;
        }
        trigger->runtimeHovered = currentHit;
        trigger->runtimeLastMouse = mousePressed;
        trigger->runtimeLastPointer = mouseInCanvasSpace;
        consumed |= trigger->pointerDown || trigger->drag;
    }
    return consumed;
}

// 塗り潰し量 (fillAmount) に応じて矩形 (pos/size) と UV を fillOrigin 方向へ削る。
// pos は左上原点・y 下向き。Left/Right は横方向、Bottom/Top は縦方向に固定辺を残す。
void ApplyFill(float amount, UIImageFillOrigin origin,
               math::Vector2& pos, math::Vector2& size,
               math::Vector2& uvMin, math::Vector2& uvMax)
{
    const float f = std::clamp(amount, 0.0f, 1.0f);
    const float du = uvMax.x - uvMin.x;
    const float dv = uvMax.y - uvMin.y;
    switch (origin) {
    case UIImageFillOrigin::Left:
        size.x  *= f;
        uvMax.x  = uvMin.x + du * f;
        break;
    case UIImageFillOrigin::Right:
        pos.x   += size.x * (1.0f - f);
        size.x  *= f;
        uvMin.x  = uvMax.x - du * f;
        break;
    case UIImageFillOrigin::Bottom:
        // 下端 (y 大 = uvMax.y) を固定し、上から削る。
        pos.y   += size.y * (1.0f - f);
        size.y  *= f;
        uvMin.y  = uvMax.y - dv * f;
        break;
    case UIImageFillOrigin::Top:
        // 上端 (y 小 = uvMin.y) を固定し、下から削る。
        size.y  *= f;
        uvMax.y  = uvMin.y + dv * f;
        break;
    }
}

// ── レンダリング ──────────────────────────────────────────────────────────────
void RenderCanvasRecursive(GameObject& go,
                           const UITransform2D& parentTransform,
                           renderer::IRenderer& renderer,
                           renderer::ResourceManager& resources,
                           UISystemContext& ctx,
                           const math::Matrix4& canvasToClip,
                           renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                           renderer::RenderLayer layer,
                           bool applySelfTransform = true)
{
    if (!go.activeInHierarchy()) return;

    UITransform2D resolved = applySelfTransform
        ? ComposeUITransform(parentTransform, go.transform)
        : parentTransform;
    auto* image  = go.GetComponent<UIImage>();
    auto* button = go.GetComponent<UIButton>();
    auto* text   = go.GetComponent<UIText>();

    // WHAT: 押下中はボタン全体を右下へ沈ませ、物理ボタンのような視覚フィードバックを出す。
    // WHY: pressedColorだけでは暗い背景画像との乗算後に差が小さくなり、入力がUIButtonまで
    //      届いたのか、Scene遷移スクリプトで止まったのかを画面上で判別できないため。
    if (button && button->enabled && button->isInteractable
        && button->state == UIButtonState::PRESSED) {
        constexpr math::Vector2 PRESSED_VISUAL_OFFSET = { 2.0f, 2.0f };
        resolved.position += Rotate2D(PRESSED_VISUAL_OFFSET, resolved.rotationZ);
    }

    if (image && image->enabled) {
        if (!image->texturePath.empty() && image->texturePath != image->loadedTexturePath) {
            std::string texturePath;
            std::string spriteName;
            const bool isSprite = asset::ParseSpriteReference(
                image->texturePath, texturePath, spriteName);
            image->texture = resources.LoadTexture(texturePath);
            image->loadedTexturePath = image->texturePath;
            image->hasResolvedSprite = false;
            image->resolvedSpriteBorder = {};
            image->resolvedTextureSize = {};

            if (isSprite) {
                asset::TextureAsset textureAsset;
                asset::TexDescSerializer serializer;
                const std::string metaPath =
                    asset::AssetManager::ResolveAssetPath(texturePath + ".meta");
                if (serializer.Load(metaPath, textureAsset)) {
                    if (const asset::SpriteRect* sprite =
                            asset::FindSprite(textureAsset.settings, spriteName)) {
                        if (const renderer::ITexture* texture = resources.Get(image->texture)) {
                            const float width = static_cast<float>(texture->GetWidth());
                            const float height = static_cast<float>(texture->GetHeight());
                            if (width > 0.0f && height > 0.0f) {
                                image->resolvedTextureSize = { width, height };
                                const float spriteWidth = sprite->width > 0
                                    ? static_cast<float>(sprite->width) : width;
                                const float spriteHeight = sprite->height > 0
                                    ? static_cast<float>(sprite->height) : height;
                                image->resolvedSpriteUvMin = {
                                    static_cast<float>(sprite->x) / width,
                                    static_cast<float>(sprite->y) / height
                                };
                                image->resolvedSpriteUvMax = {
                                    (static_cast<float>(sprite->x) + spriteWidth) / width,
                                    (static_cast<float>(sprite->y) + spriteHeight) / height
                                };
                                image->resolvedSpriteBorder = {
                                    sprite->borderLeft, sprite->borderTop,
                                    sprite->borderRight, sprite->borderBottom
                                };
                                image->hasResolvedSprite = true;
                            }
                        }
                    }
                }
            }
        }

        Rect          r     = RectFromTransform(go.transform, resolved);
        math::Vector4 color = image->color;
        if (button)
            color = ResolveButtonImageColor(color, *button);

        // 塗り潰し量に応じて矩形と UV を fillOrigin 方向へクリップする (体力ゲージ等)。
        // transform.scale は変えず描画時だけ削るので、レイアウトや当たり判定には影響しない。
        math::Vector2 uvMin = image->hasResolvedSprite
            ? image->resolvedSpriteUvMin : image->uvMin;
        math::Vector2 uvMax = image->hasResolvedSprite
            ? image->resolvedSpriteUvMax : image->uvMax;
        if (image->fillAmount < 1.0f)
            ApplyFill(image->fillAmount, image->fillOrigin, r.pos, r.size, uvMin, uvMax);

        if (r.size.x > 0.0f && r.size.y > 0.0f) {
            const auto texture = image->texture.IsValid() ? image->texture : ctx.whiteTexture;
            if (image->imageType == UIImageType::Sliced
                && image->hasResolvedSprite && image->fillAmount >= 1.0f) {
                SubmitSlicedImage(renderer, resources, ctx, canvasToClip, pso, layer,
                                  r.pos, r.size, color, uvMin, uvMax,
                                  image->resolvedSpriteBorder,
                                  image->resolvedTextureSize,
                                  texture, resolved.rotationZ);
            } else {
                SubmitImage(renderer, resources, ctx, canvasToClip, pso, layer,
                            r.pos, r.size, color, uvMin, uvMax,
                            texture, resolved.rotationZ);
            }
        }
    }

    if (text && text->enabled) {
        SubmitText(renderer, resources, ctx, canvasToClip, pso, layer, *text, resolved.position);
    }

    UITransform2D childTransform = resolved;
    if (const auto* scroll = go.GetComponent<UIScrollView>(); scroll && scroll->enabled)
        childTransform.position -= Rotate2D(scroll->scrollPosition, resolved.rotationZ);
    for (GameObject* child : SortedUIChildren(go))
        RenderCanvasRecursive(*child, childTransform, renderer, resources, ctx, canvasToClip, pso, layer);
}

// ── サブシステム ──────────────────────────────────────────────────────────────
void UILayoutSystem(const std::vector<CanvasEntry>& canvases, float viewportWidth, float viewportHeight)
{
    for (const CanvasEntry& entry : canvases) {
        const float scale = ResolveCanvasScale(*entry.canvas, viewportWidth, viewportHeight);
        ApplyUILayoutRecursive(*entry.go, scale);
    }
}

void UIEventSystem(const std::vector<CanvasEntry>& canvases,
                   float viewportWidth,
                   float viewportHeight,
                   math::Vector2 rawMouseInViewport,
                   bool mousePressed,
                   const math::Matrix4& viewProjection,
                   math::Vector3 cameraWorldPos,
                   UIRenderTargetView targetView)
{
    // Canvas 間でも描画順の逆から入力を解決し、最前面で消費された入力を背面へ渡さない。
    bool inputConsumed = false;
    for (auto canvasIt = canvases.rbegin(); canvasIt != canvases.rend(); ++canvasIt) {
        const CanvasEntry& entry = *canvasIt;
        if (!ShouldRenderCanvas(*entry.canvas, targetView))
            continue;

        // ── WorldSpace ヒット判定: スクリーン座標→ワールドレイ→キャンバスピクセル ──
        if (!IsScreenSpaceRenderMode(entry.canvas->renderMode)) {
            // 1. NDC マウス座標を計算してワールドレイを構築する
            const float ndcX =  (rawMouseInViewport.x / (std::max)(1.0f, viewportWidth))  * 2.0f - 1.0f;
            const float ndcY = -(rawMouseInViewport.y / (std::max)(1.0f, viewportHeight)) * 2.0f + 1.0f;
            const math::Matrix4 invVP = math::Matrix4::Inverse(viewProjection);
            const math::Ray worldRay = math::Ray::FromNDC(ndcX, ndcY, cameraWorldPos, invVP);

            // 2. キャンバスのワールド行列と法線平面を構築する
            const math::Matrix4 worldMat = math::Matrix4::TRS(
                entry.go->transform.worldPosition,
                entry.go->transform.worldRotation,
                math::Vector3::ONE);
            // Z 列 = キャンバス平面の法線 (ローカル前方がワールド空間でどの方向か)
            const math::Vector3 normal = {
                worldMat.m[0][2], worldMat.m[1][2], worldMat.m[2][2]
            };
            const math::Plane plane = math::Plane::FromNormalAndPoint(
                normal, entry.go->transform.worldPosition);

            // 3. レイと平面の交差判定
            float t;
            if (!worldRay.IntersectPlane(plane, t) || t < 0.0f) continue;
            const math::Vector3 hitWorld = worldRay.At(t);

            // 4. ヒット点をキャンバスピクセル座標へ逆変換する
            // worldMat^-1 を手計算: 純粋な TRS (scale=1) なら  localPos = invWorldMat * hitWorld
            const math::Matrix4 invWorld = math::Matrix4::Inverse(worldMat);
            const math::Vector4 hitLocal = MulMV(invWorld,
                { hitWorld.x, hitWorld.y, hitWorld.z, 1.0f });

            const float ws = entry.canvas->worldScale;
            const math::Vector2 canvasPx = {
                 hitLocal.x / ws + entry.canvas->canvasWidth  * 0.5f,
                -hitLocal.y / ws + entry.canvas->canvasHeight * 0.5f
            };

            const UITransform2D canvasRoot{};
            inputConsumed |= ProcessUIEventsRecursive(*entry.go, canvasRoot, canvasPx, mousePressed,
                                                      !inputConsumed, false);
            continue;
        }

        // ── ScreenSpace (Overlay / ScreenSpaceCamera) ──────────────────────────
        // NOTE: BuildCanvasRuntimeState を呼ばず直接計算する (cameraWorldRot 不要)
        float visibleW = 1.0f, visibleH = 1.0f;
        ResolveScreenSpaceCanvasArea(*entry.canvas, viewportWidth, viewportHeight, visibleW, visibleH);
        const float scaleX = (std::max)(1.0f, viewportWidth) / visibleW;
        const float scaleY = (std::max)(1.0f, viewportHeight) / visibleH;
        const math::Vector2 mouseInCanvas = {
            rawMouseInViewport.x / scaleX, rawMouseInViewport.y / scaleY
        };

        const UITransform2D canvasRoot{};
        inputConsumed |= ProcessUIEventsRecursive(*entry.go, canvasRoot, mouseInCanvas, mousePressed,
                                                  !inputConsumed, false);
    }
}

void UIRenderSystem(const std::vector<CanvasEntry>& canvases,
                    renderer::IRenderer& renderer,
                    renderer::ResourceManager& resources,
                    UISystemContext& ctx,
                    float viewportWidth,
                    float viewportHeight,
                    math::Vector2 rawMouseInViewport,
                    math::Vector3 cameraWorldPos,
                    math::Quaternion cameraWorldRot,
                    const math::Matrix4& viewProjection,
                    UIRenderTargetView targetView)
{
    renderer.SetSampler(5, renderer::SamplerMode::CLAMP_LINEAR);

    for (const CanvasEntry& entry : canvases) {
        if (!ShouldRenderCanvas(*entry.canvas, targetView))
            continue;

        const CanvasRuntimeState state = BuildCanvasRuntimeState(
            *entry.canvas, *entry.go,
            viewportWidth, viewportHeight,
            rawMouseInViewport, viewProjection,
            cameraWorldPos, cameraWorldRot, ctx, targetView);
        const UITransform2D canvasRoot{};
        RenderCanvasRecursive(*entry.go, canvasRoot, renderer, resources, ctx,
                              state.canvasToClip, state.pso, state.layer, false);
    }
}

} // namespace (anonymous)

// ── 公開 API ──────────────────────────────────────────────────────────────────
void UISystemSetDefaultFontPath(UISystemContext& ctx, const std::string& basePath)
{
    ctx.defaultFontPath = basePath;
}

void UISystemFlushCache(UISystemContext& ctx)
{
    // WHY: シーン破棄時・アセットリロード時に呼び出して、古い FontAtlas テクスチャハンドルが
    //      破棄済み ResourceManager を参照し続けるのを防ぐ。
    ctx.fontAtlasCache.clear();
}

void UISystem(Scene& scene,
              renderer::IRenderer& renderer,
              renderer::ResourceManager& resources,
              UISystemContext& ctx,
              float viewportWidth,
              float viewportHeight,
              math::Vector2 mouseInViewport,
              bool mousePressed,
              math::Vector3    cameraWorldPos,
              math::Quaternion cameraWorldRot,
              const math::Matrix4& viewProjection,
              UIRenderTargetView targetView)
{
    EnsureInit(ctx, resources);

    // 頂点バッファの貸し出しを巻き戻す。フレームが変わったときだけ行うのは、
    // 同じフレーム内で 1 つの Context が複数回描いても実体が衝突しないようにするため。
    if (ctx.lastResetFrame != fbzz::Time::frameCount) {
        ctx.lastResetFrame    = fbzz::Time::frameCount;
        ctx.imageVertexCursor = 0;
        ctx.textVertexCursor  = 0;
    }

    if (!ctx.shader.IsValid() || !ctx.constants.IsValid() ||
        !ctx.pso.IsValid() || !ctx.worldPso.IsValid() || !ctx.whiteTexture.IsValid())
        return;

    std::vector<CanvasEntry> canvases;
    CollectCanvases(scene, canvases);

    UITextSizeSystem(canvases, ctx, resources);
    UILayoutSystem(canvases, viewportWidth, viewportHeight);
    // WHY: Editorは同じSceneをScene / Game / Canvas Editorへ1フレーム中に複数回描画する。
    //      各ViewportでUIButtonのlastMouseStateやonClickを更新すると、後続の描画パスが
    //      Game Viewportで生成したクリックイベントを消してしまう。ゲーム入力を受ける
    //      GameViewportだけが共有Componentのイベント状態を更新する。
    if (targetView == UIRenderTargetView::GameViewport) {
        UIEventSystem(canvases, viewportWidth, viewportHeight, mouseInViewport,
                      mousePressed, viewProjection, cameraWorldPos, targetView);
    }
    UIRenderSystem(canvases, renderer, resources, ctx,
                   viewportWidth, viewportHeight, mouseInViewport,
                   cameraWorldPos, cameraWorldRot, viewProjection, targetView);
}

} // namespace fbzz::scene
