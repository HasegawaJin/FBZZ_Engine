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
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/FontAtlas.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/RenderLayer.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Math/Matrix4.hpp"
#include "Math/Vector4.hpp"
#include "Math/Ray.hpp"
#include "Math/Plane.hpp"
#include "Engine/Renderer/SamplerMode.hpp"
#include "Engine/Core/Logger.hpp"
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

math::Vector4 Multiply(const math::Vector4& a, const math::Vector4& b)
{
    return { a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w };
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

    ctx.imageVB = resources.CreateVertexBuffer(nullptr, kImageVBVertices * sizeof(UIVertex), sizeof(UIVertex));
    ctx.textVB  = resources.CreateVertexBuffer(nullptr, kTextVBVertices  * sizeof(UIVertex), sizeof(UIVertex));

    if (!ctx.shader.IsValid() || !ctx.textShader.IsValid() || !ctx.constants.IsValid() ||
        !ctx.pso.IsValid() || !ctx.worldPso.IsValid() ||
        !ctx.whiteTexture.IsValid() || !ctx.imageVB.IsValid() || !ctx.textVB.IsValid()) {
        FBZZ_LOG_ERROR("UISystem init failed: shader=%d textShader=%d cb=%d pso=%d worldPso=%d white=%d imageVB=%d textVB=%d",
                       static_cast<int>(ctx.shader.IsValid()),
                       static_cast<int>(ctx.textShader.IsValid()),
                       static_cast<int>(ctx.constants.IsValid()),
                       static_cast<int>(ctx.pso.IsValid()),
                       static_cast<int>(ctx.worldPso.IsValid()),
                       static_cast<int>(ctx.whiteTexture.IsValid()),
                       static_cast<int>(ctx.imageVB.IsValid()),
                       static_cast<int>(ctx.textVB.IsValid()));
    }
}

// ── Canvas 収集 ──────────────────────────────────────────────────────────────
void CollectCanvasesRecursive(GameObject* go, std::vector<CanvasEntry>& canvases)
{
    if (!go || !go->activeSelf()) return;
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

        // WHY: 再帰収集後は Canvas が子 GO に置かれる可能性があるため worldPosition を使う
        const math::Matrix4 worldMatrix = math::Matrix4::TRS(
            canvasGO.transform.worldPosition,
            canvasGO.transform.worldRotation,
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
void UpdateButton(UIButton& button, const Rect& rect, math::Vector2 mouse, bool mousePressed)
{
    const UIButtonState previousState = button.state;
    button.onClick = false;
    button.onEnter = false;
    button.onExit  = false;

    if (!button.enabled || !button.isInteractable) {
        button.state            = UIButtonState::NORMAL;
        button.wasPressedOnThis = false;
        button.lastMouseState   = mousePressed;
        return;
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
}

math::Vector4 ButtonTint(const UIButton& button)
{
    if (!button.enabled || !button.isInteractable) return button.disabledColor;
    if (button.state == UIButtonState::PRESSED) return button.pressedColor;
    if (button.state == UIButtonState::HOVERED) return button.hoverColor;
    return button.normalColor;
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
    resources.Update(ctx.imageVB, vertices, sizeof(vertices));

    UIConstants constants{};
    constants.ortho  = canvasToClip;
    constants.color  = color;
    constants.uvRect = { uvMin.x, uvMin.y, uvMax.x, uvMax.y };
    resources.Update(ctx.constants, &constants, sizeof(constants));

    renderer::DrawCall call;
    call.vertexBuffer       = ctx.imageVB;
    call.shader             = ctx.shader;
    call.pipelineState      = pso;
    call.constantBuffers[0] = ctx.constants;
    call.vertexCount        = 6;
    call.layer              = layer;
    call.topology           = renderer::PrimitiveTopology::TRIANGLE_LIST;
    call.textures[0]        = texture.IsValid() ? texture : ctx.whiteTexture;
    renderer.Submit(call, resources);
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

math::Vector2 ComputeTextLogicalSize(const UIText& text,
                                     UISystemContext& ctx,
                                     renderer::ResourceManager& resources)
{
    const std::string& path = text.fontPath.empty() ? ctx.defaultFontPath : text.fontPath;
    renderer::FontAtlas& atlas = GetOrLoadFontAtlas(path, ctx, resources);
    if (!atlas.IsValid()) return {};

    const float scale = text.fontSize / atlas.GetLineHeight();
    const float cellW = atlas.GetCellW() * scale;
    const float cellH = atlas.GetLineHeight() * scale;

    float maxW = 0.0f, lineW = 0.0f;
    float totalH = cellH;
    for (char c : text.text) {
        if (c == '\n') {
            maxW  = (std::max)(maxW, lineW);
            lineW = 0.0f;
            totalH += cellH;
            continue;
        }
        const renderer::FontGlyph* g = atlas.GetGlyph(c);
        lineW += g ? (g->advance * scale + text.letterSpacing) : (cellW * 0.5f + text.letterSpacing);
    }
    maxW = (std::max)(maxW, lineW);
    return { maxW, totalH };
}

void UpdateTextSizesRecursive(GameObject& go,
                              UISystemContext& ctx,
                              renderer::ResourceManager& resources)
{
    if (!go.activeSelf()) return;
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

    const float scale = text.fontSize / atlas.GetLineHeight();
    const float cellW = atlas.GetCellW() * scale;
    const float cellH = atlas.GetLineHeight() * scale;

    // Center / Right 整列のために行ごとの幅を事前計算する
    std::vector<float> lineWidths;
    if (text.align != TextAlign::Left) {
        float lineW = 0.0f;
        for (char c : text.text) {
            if (c == '\n') { lineWidths.push_back(lineW); lineW = 0.0f; continue; }
            const renderer::FontGlyph* g = atlas.GetGlyph(c);
            lineW += g ? (g->advance * scale + text.letterSpacing) : (cellW * 0.5f + text.letterSpacing);
        }
        lineWidths.push_back(lineW);
    }

    int lineIdx = 0;
    auto lineStartX = [&]() -> float {
        if (text.align == TextAlign::Left) return position.x;
        const float lw = lineIdx < static_cast<int>(lineWidths.size()) ? lineWidths[lineIdx] : 0.0f;
        if (text.align == TextAlign::Center) return position.x - lw * 0.5f;
        return position.x - lw; // Right
    };

    math::Vector2 pen = { lineStartX(), position.y };

    std::vector<UIVertex> verts;
    verts.reserve(text.text.size() * 6);

    for (char c : text.text) {
        if (c == '\n') {
            ++lineIdx;
            pen.x = lineStartX();
            pen.y += cellH;
            continue;
        }

        const renderer::FontGlyph* g = atlas.GetGlyph(c);
        if (!g) {
            pen.x += cellW * 0.5f + text.letterSpacing;
            continue;
        }

        const float x  = pen.x;
        const float y  = pen.y;
        const float x2 = x + cellW;
        const float y2 = y + cellH;

        verts.push_back({ {x,  y},  { g->u0, g->v0 } });
        verts.push_back({ {x,  y2}, { g->u0, g->v1 } });
        verts.push_back({ {x2, y},  { g->u1, g->v0 } });
        verts.push_back({ {x2, y},  { g->u1, g->v0 } });
        verts.push_back({ {x,  y2}, { g->u0, g->v1 } });
        verts.push_back({ {x2, y2}, { g->u1, g->v1 } });

        pen.x += g->advance * scale + text.letterSpacing;
    }

    if (verts.empty()) return;

    // 定数バッファは全チャンク共通なので 1 回だけ更新する。
    UIConstants constants{};
    constants.ortho  = canvasToClip;
    constants.color  = text.color;
    constants.uvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
    resources.Update(ctx.constants, &constants, sizeof(constants));

    renderer::DrawCall call;
    call.vertexBuffer       = ctx.textVB;
    // WHY: UIText.hlsl の .r チャンネルを coverage として使い、alpha チャンネル依存を排除する。
    call.shader             = ctx.textShader;
    call.pipelineState      = pso;
    call.constantBuffers[0] = ctx.constants;
    call.layer              = layer;
    call.topology           = renderer::PrimitiveTopology::TRIANGLE_LIST;
    call.textures[0]        = atlas.GetTexture();

    // kTextVBVertices を超えるテキストはチャンク分割して複数ドローコールで描く。
    // WHY: 固定 VB サイズを超えてもエラーで打ち切らず全グリフを描画するため。
    uint32_t offset = 0;
    const uint32_t total = static_cast<uint32_t>(verts.size());
    while (offset < total) {
        const uint32_t chunk = (std::min)(total - offset, kTextVBVertices);
        resources.Update(ctx.textVB, verts.data() + offset, chunk * sizeof(UIVertex));
        call.vertexCount = chunk;
        renderer.Submit(call, resources);
        offset += chunk;
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
        if (!child || !child->activeSelf()) continue;
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
    if (!go.activeSelf()) return;

    if (auto* layout = go.GetComponent<UILayoutGroup>(); layout && layout->enabled)
        ApplyLayout(go, *layout, canvasScale);

    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i))
            ApplyUILayoutRecursive(*child, canvasScale);
    }
}

// ── UIButton イベント処理 ─────────────────────────────────────────────────────
void ProcessUIEventsRecursive(GameObject& go,
                              const UITransform2D& parentTransform,
                              math::Vector2 mouseInCanvasSpace,
                              bool mousePressed,
                              bool applySelfTransform = true)
{
    if (!go.activeSelf()) return;

    const UITransform2D resolved = applySelfTransform
        ? ComposeUITransform(parentTransform, go.transform)
        : parentTransform;

    auto* button = go.GetComponent<UIButton>();
    if (button) {
        // UIImage の有無に関わらず transform.scale.xy が有効ならヒット判定する。
        // WHY: UIImage なしで UIText / 子要素だけで構成されるボタンにも対応する。
        if (go.transform.scale.x > 0.0f && go.transform.scale.y > 0.0f)
            UpdateButton(*button, RectFromTransform(go.transform, resolved), mouseInCanvasSpace, mousePressed);
        else {
            button->onClick          = false;
            button->onEnter          = false;
            button->onExit           = false;
            button->state            = UIButtonState::NORMAL;
            button->wasPressedOnThis = false;
            button->lastMouseState   = mousePressed;
        }
    }

    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i))
            ProcessUIEventsRecursive(*child, resolved, mouseInCanvasSpace, mousePressed);
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
    if (!go.activeSelf()) return;

    const UITransform2D resolved = applySelfTransform
        ? ComposeUITransform(parentTransform, go.transform)
        : parentTransform;
    auto* image  = go.GetComponent<UIImage>();
    auto* button = go.GetComponent<UIButton>();
    auto* text   = go.GetComponent<UIText>();

    if (image && image->enabled) {
        if (!image->texturePath.empty() && image->texturePath != image->loadedTexturePath) {
            image->texture           = resources.LoadTexture(image->texturePath);
            image->loadedTexturePath = image->texturePath;
        }

        const Rect    r     = RectFromTransform(go.transform, resolved);
        math::Vector4 color = image->color;
        if (button)
            color = Multiply(color, ButtonTint(*button));
        SubmitImage(renderer, resources, ctx, canvasToClip, pso, layer,
                    r.pos, r.size, color,
                    image->uvMin, image->uvMax,
                    image->texture.IsValid() ? image->texture : ctx.whiteTexture,
                    resolved.rotationZ);
    }

    if (text && text->enabled) {
        SubmitText(renderer, resources, ctx, canvasToClip, pso, layer, *text, resolved.position);
    }

    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i))
            RenderCanvasRecursive(*child, resolved, renderer, resources, ctx, canvasToClip, pso, layer);
    }
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
    for (const CanvasEntry& entry : canvases) {
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
            ProcessUIEventsRecursive(*entry.go, canvasRoot, canvasPx, mousePressed, false);
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
        ProcessUIEventsRecursive(*entry.go, canvasRoot, mouseInCanvas, mousePressed, false);
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
    if (!ctx.shader.IsValid() || !ctx.constants.IsValid() ||
        !ctx.pso.IsValid() || !ctx.worldPso.IsValid() ||
        !ctx.whiteTexture.IsValid() || !ctx.imageVB.IsValid() || !ctx.textVB.IsValid())
        return;

    std::vector<CanvasEntry> canvases;
    CollectCanvases(scene, canvases);

    UITextSizeSystem(canvases, ctx, resources);
    UILayoutSystem(canvases, viewportWidth, viewportHeight);
    UIEventSystem(canvases, viewportWidth, viewportHeight, mouseInViewport,
                  mousePressed, viewProjection, cameraWorldPos, targetView);
    UIRenderSystem(canvases, renderer, resources, ctx,
                   viewportWidth, viewportHeight, mouseInViewport,
                   cameraWorldPos, cameraWorldRot, viewProjection, targetView);
}

} // namespace fbzz::scene
