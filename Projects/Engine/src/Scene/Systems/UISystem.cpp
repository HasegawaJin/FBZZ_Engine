// FBZZ Engine
// UISystem.cpp | fbzz::scene
// ランタイム UI の描画とボタン入力処理
// UICanvas / UIImage / UIText / UIButton を走査し、DrawCall と hit 状態を作る。
// ScreenSpace と WorldSpace の両方を扱う。
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
#include "Engine/Renderer/SamplerMode.hpp"
#include "Engine/Core/Logger.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
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

std::string s_defaultFontPath = "Assets/Fonts/Default/Roboto/Roboto-VariableFont_wdth,wght";

renderer::ResourceHandle<renderer::ShaderTag>         s_shader;
renderer::ResourceHandle<renderer::ShaderTag>         s_textShader; // TTF アトラス用 (.r チャンネルを coverage として使う)
renderer::ResourceHandle<renderer::ConstantBufferTag> s_constants;
renderer::ResourceHandle<renderer::PipelineStateTag>  s_pso;      // ScreenSpace: 深度テストなし
renderer::ResourceHandle<renderer::PipelineStateTag>  s_worldPso; // WorldSpace: 深度テストあり (DEPTH_READ)
renderer::ResourceHandle<renderer::TextureTag>        s_whiteTexture;
renderer::ResourceHandle<renderer::BufferTag>         s_imageVB;
renderer::ResourceHandle<renderer::BufferTag>         s_textVB;

// フォントアトラスキャッシュ。
// WHY: UIText ごとに毎フレームロードすると IO コストが爆発するため、
//      basePath をキーに初回ロード後はキャッシュから返す。
std::unordered_map<std::string, renderer::FontAtlas> s_fontAtlasCache;

// Fixed VB capacities
static constexpr uint32_t kImageVBVertices = 6;
static constexpr uint32_t kTextVBVertices  = 4096; // up to ~682 glyphs

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
    return { resolved.position,
             { t.scale.x,    t.scale.y    } };
}

math::Vector4 Multiply(const math::Vector4& a, const math::Vector4& b)
{
    return { a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w };
}

void EnsureInit(renderer::ResourceManager& resources)
{
    if (s_shader.IsValid()) return;

    s_shader     = resources.LoadShader("Assets/shaders/UI/UISprite.hlsl");
    s_textShader = resources.LoadShader("Assets/shaders/UI/UIText.hlsl");
    s_constants  = resources.CreateConstantBuffer(sizeof(UIConstants));
    s_pso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_OFF  // ScreenSpace UI は深度を無視して常に最前面
    });
    // WorldSpace UI: 深度テストあり (3D オブジェクトで遮蔽される)・深度書き込みなし
    // (アルファブレンドと両立させるため書き込みは行わない)
    s_worldPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });

    static constexpr uint8_t kWhite[4] = { 255, 255, 255, 255 };
    s_whiteTexture = resources.CreateTexture(kWhite, 1, 1);

    // 頂点バッファは一度だけ確保し、毎フレーム Update で上書きする。
    s_imageVB = resources.CreateVertexBuffer(nullptr, kImageVBVertices * sizeof(UIVertex), sizeof(UIVertex));
    s_textVB  = resources.CreateVertexBuffer(nullptr, kTextVBVertices  * sizeof(UIVertex), sizeof(UIVertex));

    if (!s_shader.IsValid() || !s_textShader.IsValid() || !s_constants.IsValid() ||
        !s_pso.IsValid() || !s_worldPso.IsValid() ||
        !s_whiteTexture.IsValid() || !s_imageVB.IsValid() || !s_textVB.IsValid()) {
        FBZZ_LOG_ERROR("UISystem init failed: shader=%d textShader=%d cb=%d pso=%d worldPso=%d white=%d imageVB=%d textVB=%d",
                       static_cast<int>(s_shader.IsValid()),
                       static_cast<int>(s_textShader.IsValid()),
                       static_cast<int>(s_constants.IsValid()),
                       static_cast<int>(s_pso.IsValid()),
                       static_cast<int>(s_worldPso.IsValid()),
                       static_cast<int>(s_whiteTexture.IsValid()),
                       static_cast<int>(s_imageVB.IsValid()),
                       static_cast<int>(s_textVB.IsValid()));
    }
}

void CollectCanvases(Scene& scene, std::vector<CanvasEntry>& canvases)
{
    const auto& roots = scene.GetRootGameObjects();
    for (GameObject* root : roots) {
        if (!root || !root->activeSelf()) continue;

        auto* canvas = root->GetComponent<UICanvas>();
        if (!canvas || !canvas->enabled) continue;

        canvases.push_back({ root, canvas });
    }

    std::sort(canvases.begin(), canvases.end(),
        [](const CanvasEntry& a, const CanvasEntry& b) {
            return a.canvas->sortOrder < b.canvas->sortOrder;
        });
}

bool IsScreenSpaceRenderMode(UIRenderMode mode);

bool ShouldRenderCanvas(const UICanvas& canvas, UIRenderTargetView targetView)
{
    // WHY: UIViewport は Unity / Unreal の UI Designer に近い Canvas Editor として扱う。
    //      WorldSpace Canvas は 3D シーン内の UI なので、UI 専用編集ビューへ混ぜると
    //      HUD / メニュー編集時に奥行きやカメラ依存の情報が混在して役割が曖昧になる。
    // WHAT: GameViewport は最終出力として全 Canvas、SceneViewport は WorldSpace、
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
    // WHY: Unity の Canvas Scaler と同じ考え方で、基準解像度と現在 Viewport の差を
    //      UI 座標変換に集約する。個々の UIImage / UIText の値を書き換えないため、
    //      編集データは常に reference 解像度の座標として保てる。
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
    // WHY: ScreenSpace UI の編集値は Canvas の論理ピクセル座標として保存する。
    //      ConstantPixelSize で Viewport 実ピクセルを座標系にしてしまうと、UI Viewport で
    //      配置した値が Play 時の Game View サイズに依存してずれる。
    // WHAT: ConstantPixelSize は canvasWidth/canvasHeight をそのまま使い、
    //       ScaleWithScreenSize は Canvas Scaler 後に見える論理範囲へ変換する。
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
                                           UIRenderTargetView targetView)
{
    (void)targetView;
    CanvasRuntimeState state{};
    if (canvas.renderMode == UIRenderMode::WorldSpace) {
        const float ws = canvas.worldScale;

        math::Matrix4 pixelToLocal = math::Matrix4::Identity();
        pixelToLocal.m[0][0] =  ws;
        pixelToLocal.m[1][1] = -ws;
        pixelToLocal.m[0][3] = -canvas.canvasWidth  * 0.5f * ws;
        pixelToLocal.m[1][3] =  canvas.canvasHeight * 0.5f * ws;

        const math::Matrix4 worldMatrix = math::Matrix4::TRS(
            canvasGO.transform.position,
            canvasGO.transform.rotation,
            math::Vector3::ONE
        );

        state.canvasToClip = viewProjection * worldMatrix * pixelToLocal;
        state.mouseInCanvasSpace = rawMouseInViewport;
        state.pso = s_worldPso;
        state.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
        return state;
    }

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
    state.pso = s_pso;
    state.layer = renderer::RenderLayer::OVERLAY_LAYER;
    return state;
}

void UpdateButton(UIButton& button, const Rect& rect, math::Vector2 mouse, bool mousePressed)
{
    const UIButtonState previousState = button.state;
    button.onClick = false;
    button.onEnter = false;
    button.onExit  = false;

    if (!button.enabled || !button.isInteractable) {
        button.state = UIButtonState::NORMAL;
        return;
    }

    const bool hit = mouse.x >= rect.pos.x && mouse.x <= rect.pos.x + rect.size.x
                  && mouse.y >= rect.pos.y && mouse.y <= rect.pos.y + rect.size.y;

    if (hit && mousePressed) {
        button.state   = UIButtonState::PRESSED;
    } else if (hit) {
        button.state   = UIButtonState::HOVERED;
        button.onClick = previousState == UIButtonState::PRESSED;
    } else {
        button.state   = UIButtonState::NORMAL;
    }

    button.onEnter = previousState == UIButtonState::NORMAL  && button.state != UIButtonState::NORMAL;
    button.onExit  = previousState != UIButtonState::NORMAL  && button.state == UIButtonState::NORMAL;
}

math::Vector4 ButtonTint(const UIButton& button)
{
    if (!button.enabled || !button.isInteractable) return button.normalColor;
    if (button.state == UIButtonState::PRESSED) return button.pressedColor;
    if (button.state == UIButtonState::HOVERED) return button.hoverColor;
    return button.normalColor;
}

// canvasToClip: キャンバスピクセル座標をクリップ空間へ変換する行列。
//   ScreenSpace: Matrix4::Orthographic(0, W, H, 0, 0, 1)
//   WorldSpace:  VP * TRS(worldPos, worldRot, ONE) * pixelToLocal
void SubmitImage(renderer::IRenderer& renderer,
                 renderer::ResourceManager& resources,
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
        { rot(-hW, -hH), { 0.0f, 0.0f } },
        { rot(-hW,  hH), { 0.0f, 1.0f } },
        { rot( hW, -hH), { 1.0f, 0.0f } },
        { rot( hW, -hH), { 1.0f, 0.0f } },
        { rot(-hW,  hH), { 0.0f, 1.0f } },
        { rot( hW,  hH), { 1.0f, 1.0f } },
    };
    resources.Update(s_imageVB, vertices, sizeof(vertices));

    UIConstants constants{};
    constants.ortho  = canvasToClip;
    constants.color  = color;
    constants.uvRect = { uvMin.x, uvMin.y, uvMax.x, uvMax.y };
    resources.Update(s_constants, &constants, sizeof(constants));

    renderer::DrawCall call;
    call.vertexBuffer       = s_imageVB;
    call.shader             = s_shader;
    call.pipelineState      = pso;
    call.constantBuffers[0] = s_constants;
    call.vertexCount        = 6;
    call.layer              = layer;
    call.topology           = renderer::PrimitiveTopology::TRIANGLE_LIST;
    call.textures[0]        = texture.IsValid() ? texture : s_whiteTexture;
    renderer.Submit(call, resources);
}

void SubmitRect(renderer::IRenderer& renderer,
                renderer::ResourceManager& resources,
                const math::Matrix4& canvasToClip,
                renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                renderer::RenderLayer layer,
                math::Vector2 position,
                math::Vector2 size,
                const math::Vector4& color)
{
    SubmitImage(renderer, resources, canvasToClip, pso, layer,
                position, size, color,
                { 0.0f, 0.0f }, { 1.0f, 1.0f }, s_whiteTexture);
}

// TTF アトラスをキャッシュから取得し、未ロードなら初回ロードする。
// IsValid() == false の場合は内蔵 SDF へフォールバックすること。
renderer::FontAtlas& GetOrLoadFontAtlas(const std::string& basePath,
                                        renderer::ResourceManager& resources)
{
    auto it = s_fontAtlasCache.find(basePath);
    if (it != s_fontAtlasCache.end())
        return it->second;

    renderer::FontAtlas& atlas = s_fontAtlasCache[basePath];
    if (!atlas.Load(basePath, resources))
        FBZZ_LOG_ERROR("UISystem: failed to load FontAtlas: %s", basePath.c_str());

    return atlas;
}

// TTF 由来のフォントアトラスを使ってテキストをサブミットする。
// WHY: 内蔵 SDF は 5x7 ピクセルビットマップが限界だが、Kenney 等の TTF アトラスは
//      任意フォントサイズで高品質な文字を描画できる。
// WHAT:
//   scale = fontSize / atlas.line_height でアトラスピクセルを論理ピクセルへ変換する。
//   各グリフのセル UV を使い、advance でペンを進める。透明部分はアルファブレンドで消える。
void SubmitTextWithAtlas(renderer::IRenderer& renderer,
                         renderer::ResourceManager& resources,
                         const math::Matrix4& canvasToClip,
                         renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                         renderer::RenderLayer layer,
                         const UIText& text,
                         math::Vector2 position)
{
    renderer::FontAtlas& atlas = GetOrLoadFontAtlas(text.fontPath, resources);
    if (!atlas.IsValid()) return;

    // アトラスのレンダリングサイズから論理ピクセルへのスケール係数
    const float scale     = text.fontSize / atlas.GetLineHeight();
    const float cellW     = atlas.GetCellW() * scale;
    const float cellH     = atlas.GetLineHeight() * scale;
    math::Vector2 pen     = position;

    std::vector<UIVertex> verts;
    verts.reserve(text.text.size() * 6);

    for (char c : text.text) {
        if (c == '\n') {
            pen.x = position.x;
            pen.y += cellH;
            continue;
        }

        const renderer::FontGlyph* g = atlas.GetGlyph(c);
        if (!g) {
            // 未登録文字: スペース幅相当だけ進める
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

    const uint32_t vertCount = static_cast<uint32_t>(verts.size());
    if (vertCount > kTextVBVertices) {
        FBZZ_LOG_ERROR("UISystem: text is too long (%u verts > %u capacity)", vertCount, kTextVBVertices);
        return;
    }
    resources.Update(s_textVB, verts.data(), vertCount * sizeof(UIVertex));

    UIConstants constants{};
    constants.ortho  = canvasToClip;
    constants.color  = text.color;
    constants.uvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
    resources.Update(s_constants, &constants, sizeof(constants));

    renderer::DrawCall call;
    call.vertexBuffer       = s_textVB;
    // WHY: UISprite.hlsl はテクスチャの alpha チャンネルをそのまま出力するが、
    //      WIC の PNG ロードが alpha を失うと背景が黒矩形として描画される。
    //      UIText.hlsl は .r チャンネルを coverage として読み clip() で背景を除去するため、
    //      alpha チャンネルの保持に依存せず正しく透明を扱える。
    call.shader             = s_textShader;
    call.pipelineState      = pso;
    call.constantBuffers[0] = s_constants;
    call.vertexCount        = vertCount;
    call.layer              = layer;
    call.topology           = renderer::PrimitiveTopology::TRIANGLE_LIST;
    call.textures[0]        = atlas.GetTexture();
    renderer.Submit(call, resources);
}

void SubmitText(renderer::IRenderer& renderer,
                renderer::ResourceManager& resources,
                const math::Matrix4& canvasToClip,
                renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                renderer::RenderLayer layer,
                const UIText& text,
                math::Vector2 position)
{
    if (!text.enabled || text.text.empty() || text.fontSize <= 0.0f) return;

    if (text.fontPath.empty()) {
        UIText defaulted = text;
        defaulted.fontPath = s_defaultFontPath;
        SubmitTextWithAtlas(renderer, resources, canvasToClip, pso, layer, defaulted, position);
        return;
    }
    SubmitTextWithAtlas(renderer, resources, canvasToClip, pso, layer, text, position);
}

// UILayoutGroup が管理する子 GO の transform.localPosition を上書きする
void ApplyLayout(GameObject& go, const UILayoutGroup& layout)
{
    const int count = go.GetChildCount();

    std::vector<int> indices;
    indices.reserve(count);
    for (int i = 0; i < count; ++i) {
        GameObject* child = go.GetChild(i);
        if (!child || !child->activeSelf()) continue;
        if (auto* img = child->GetComponent<UIImage>(); img && img->enabled)
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
}

void ApplyUILayoutRecursive(GameObject& go)
{
    if (!go.activeSelf()) return;

    if (auto* layout = go.GetComponent<UILayoutGroup>(); layout && layout->enabled)
        ApplyLayout(go, *layout);

    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i))
            ApplyUILayoutRecursive(*child);
    }
}

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
    auto* image  = go.GetComponent<UIImage>();
    auto* button = go.GetComponent<UIButton>();
    if (button) {
        if (image && image->enabled)
            UpdateButton(*button, RectFromTransform(go.transform, resolved), mouseInCanvasSpace, mousePressed);
        else {
            button->onClick = false;
            button->onEnter = false;
            button->onExit  = false;
            button->state   = UIButtonState::NORMAL;
        }
    }

    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i))
            ProcessUIEventsRecursive(*child, resolved, mouseInCanvasSpace, mousePressed);
    }
}

// canvasToClip: SubmitImage / SubmitText へそのまま渡す変換行列。
// pso / layer:  ScreenSpace と WorldSpace で異なる PSO とレイヤーを切り替える。
void RenderCanvasRecursive(GameObject& go,
                           const UITransform2D& parentTransform,
                           renderer::IRenderer& renderer,
                           renderer::ResourceManager& resources,
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
        if (!image->texturePath.empty())
            image->texture = resources.LoadTexture(image->texturePath);

        const Rect    r     = RectFromTransform(go.transform, resolved);
        math::Vector4 color = image->color;
        if (button)
            color = Multiply(color, ButtonTint(*button));
        SubmitImage(renderer, resources, canvasToClip, pso, layer,
                    r.pos, r.size, color,
                    image->uvMin, image->uvMax,
                    image->texture.IsValid() ? image->texture : s_whiteTexture,
                    resolved.rotationZ);
    }

    if (text && text->enabled) {
        SubmitText(renderer, resources, canvasToClip, pso, layer, *text, resolved.position);
    }

    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i))
            RenderCanvasRecursive(*child, resolved, renderer, resources, canvasToClip, pso, layer);
    }
}

void UILayoutSystem(const std::vector<CanvasEntry>& canvases)
{
    for (const CanvasEntry& entry : canvases)
        ApplyUILayoutRecursive(*entry.go);
}

void UIEventSystem(const std::vector<CanvasEntry>& canvases,
                   float viewportWidth,
                   float viewportHeight,
                   math::Vector2 rawMouseInViewport,
                   bool mousePressed,
                   const math::Matrix4& viewProjection,
                   UIRenderTargetView targetView)
{
    for (const CanvasEntry& entry : canvases) {
        if (!ShouldRenderCanvas(*entry.canvas, targetView))
            continue;
        if (!IsScreenSpaceRenderMode(entry.canvas->renderMode))
            continue;

        const CanvasRuntimeState state = BuildCanvasRuntimeState(
            *entry.canvas, *entry.go, viewportWidth, viewportHeight, rawMouseInViewport,
            viewProjection, targetView);
        const UITransform2D canvasRoot{};
        ProcessUIEventsRecursive(*entry.go, canvasRoot, state.mouseInCanvasSpace, mousePressed, false);
    }
}

void UIRenderSystem(const std::vector<CanvasEntry>& canvases,
                    renderer::IRenderer& renderer,
                    renderer::ResourceManager& resources,
                    float viewportWidth,
                    float viewportHeight,
                    math::Vector2 rawMouseInViewport,
                    const math::Matrix4& viewProjection,
                    UIRenderTargetView targetView)
{
    renderer.SetSampler(5, renderer::SamplerMode::CLAMP_LINEAR);

    for (const CanvasEntry& entry : canvases) {
        if (!ShouldRenderCanvas(*entry.canvas, targetView))
            continue;

        const CanvasRuntimeState state = BuildCanvasRuntimeState(
            *entry.canvas, *entry.go, viewportWidth, viewportHeight, rawMouseInViewport,
            viewProjection, targetView);
        const UITransform2D canvasRoot{};
        RenderCanvasRecursive(*entry.go, canvasRoot, renderer, resources,
                              state.canvasToClip, state.pso, state.layer, false);
    }
}

} // namespace

void UISystemSetDefaultFontPath(const std::string& basePath)
{
    s_defaultFontPath = basePath;
}

void UISystem(Scene& scene,
              renderer::IRenderer& renderer,
              renderer::ResourceManager& resources,
              float viewportWidth,
              float viewportHeight,
              math::Vector2 mouseInCanvasSpace,
              bool mousePressed,
              const math::Matrix4& viewProjection,
              UIRenderTargetView targetView)
{
    EnsureInit(resources);
    if (!s_shader.IsValid() || !s_constants.IsValid() ||
        !s_pso.IsValid() || !s_worldPso.IsValid() ||
        !s_whiteTexture.IsValid() || !s_imageVB.IsValid() || !s_textVB.IsValid())
        return;

    std::vector<CanvasEntry> canvases;
    CollectCanvases(scene, canvases);

    UILayoutSystem(canvases);
    UIEventSystem(canvases, viewportWidth, viewportHeight, mouseInCanvasSpace,
                  mousePressed, viewProjection, targetView);
    UIRenderSystem(canvases, renderer, resources, viewportWidth, viewportHeight,
                   mouseInCanvasSpace, viewProjection, targetView);
}

} // namespace fbzz::scene
