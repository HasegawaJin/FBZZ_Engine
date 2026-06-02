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
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/RenderLayer.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Math/Matrix4.hpp"
#include "Math/Vector4.hpp"
#include "Engine/Renderer/SamplerMode.hpp"
#include "Engine/Core/Logger.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
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

using Glyph = std::array<uint8_t, 7>;

// 前方宣言。完全なグリフテーブルと同じ場所で定義する。
Glyph GetGlyph(char c);

// --- SDF フォントアトラス ---
// ASCII 0x20..0x7E の 95 文字を、各 kGlyphSize x kGlyphSize の SDF texel として描画する。
// アトラスは kAtlasCols 列で、必要な行数ぶん並べる。
static constexpr int kGlyphSize  = 24;
static constexpr int kAtlasCols  = 10;
static constexpr int kAtlasRows  = 10; // 10x10 = 100 slots >= 95 chars
static constexpr int kAtlasW     = kGlyphSize * kAtlasCols; // 240
static constexpr int kAtlasH     = kGlyphSize * kAtlasRows; // 240
static constexpr int kFirstChar  = 0x20; // ' '
static constexpr int kLastChar   = 0x7E; // '~'
static constexpr int kNumGlyphs  = kLastChar - kFirstChar + 1; // 95

// UV テーブル。char - kFirstChar を添字にする。
struct GlyphUV { math::Vector2 uvMin, uvMax; };
GlyphUV s_glyphUVs[kNumGlyphs];

// 既存の GetGlyph() テーブルから 5x7 の文字ビットマップを作る。
// 点灯している行・列を [7][5] の bool 配列へ詰める。
void BitmapForChar(char c, bool out[7][5])
{
    Glyph g = GetGlyph(c);
    for (int row = 0; row < 7; ++row)
        for (int col = 0; col < 5; ++col)
            out[row][col] = (g[row] & (1u << (4 - col))) != 0;
}

// kGlyphSize x kGlyphSize の単一 SDF グリフを out へ生成する。値は grayscale uint8。
// spread は SDF が出力ピクセル上で伸びる最大距離。
void GenerateGlyphSDF(char c, uint8_t* out, int spread = 4)
{
    bool bitmap[7][5];
    BitmapForChar(c, bitmap);

    // ビットマップを kGlyphSize x kGlyphSize の bool グリッドへ拡大する。
    // ビットマップセル (col, row) は次の出力ピクセル範囲に対応する。
    // [col*kGlyphSize/5, (col+1)*kGlyphSize/5) x [row*kGlyphSize/7, (row+1)*kGlyphSize/7).
    bool expanded[kGlyphSize][kGlyphSize] = {};
    for (int py = 0; py < kGlyphSize; ++py) {
        int brow = py * 7 / kGlyphSize;
        for (int px = 0; px < kGlyphSize; ++px) {
            int bcol = px * 5 / kGlyphSize;
            expanded[py][px] = bitmap[brow][bcol];
        }
    }

    for (int py = 0; py < kGlyphSize; ++py) {
        for (int px = 0; px < kGlyphSize; ++px) {
            float minDist = static_cast<float>(spread + 1);
            bool inside = expanded[py][px];
            // spread 半径内で内外が逆のピクセルを探す。
            for (int sy = py - spread; sy <= py + spread; ++sy) {
                if (sy < 0 || sy >= kGlyphSize) continue;
                for (int sx = px - spread; sx <= px + spread; ++sx) {
                    if (sx < 0 || sx >= kGlyphSize) continue;
                    if (expanded[sy][sx] != inside) {
                        float d = std::sqrt(static_cast<float>((sx - px) * (sx - px) + (sy - py) * (sy - py)));
                        if (d < minDist) minDist = d;
                    }
                }
            }
            float sdf = inside
                ? 0.5f + 0.5f * std::min(minDist, static_cast<float>(spread)) / static_cast<float>(spread)
                : 0.5f - 0.5f * std::min(minDist, static_cast<float>(spread)) / static_cast<float>(spread);
            out[py * kGlyphSize + px] = static_cast<uint8_t>(std::clamp(sdf * 255.0f, 0.0f, 255.0f));
        }
    }
}

// Build the full RGBA atlas and return it as a flat RGBA vector.
std::vector<uint8_t> BuildFontAtlas()
{
    std::vector<uint8_t> atlas(kAtlasW * kAtlasH * 4, 0);

    uint8_t glyphBuf[kGlyphSize * kGlyphSize];
    for (int i = 0; i < kNumGlyphs; ++i) {
        char c = static_cast<char>(kFirstChar + i);
        GenerateGlyphSDF(c, glyphBuf);

        int col = i % kAtlasCols;
        int row = i / kAtlasCols;
        int ox  = col * kGlyphSize;
        int oy  = row * kGlyphSize;

        for (int py = 0; py < kGlyphSize; ++py) {
            for (int px = 0; px < kGlyphSize; ++px) {
                int atlasIdx = ((oy + py) * kAtlasW + (ox + px)) * 4;
                uint8_t v = glyphBuf[py * kGlyphSize + px];
                atlas[atlasIdx + 0] = v;
                atlas[atlasIdx + 1] = v;
                atlas[atlasIdx + 2] = v;
                atlas[atlasIdx + 3] = v;
            }
        }

        float u0 = static_cast<float>(ox) / kAtlasW;
        float v0 = static_cast<float>(oy) / kAtlasH;
        float u1 = static_cast<float>(ox + kGlyphSize) / kAtlasW;
        float v1 = static_cast<float>(oy + kGlyphSize) / kAtlasH;
        s_glyphUVs[i] = { { u0, v0 }, { u1, v1 } };
    }
    return atlas;
}

renderer::ResourceHandle<renderer::ShaderTag>        s_shader;
renderer::ResourceHandle<renderer::ShaderTag>        s_textShader;
renderer::ResourceHandle<renderer::ConstantBufferTag> s_constants;
renderer::ResourceHandle<renderer::PipelineStateTag>  s_pso;       // ScreenSpace: 深度テストなし
renderer::ResourceHandle<renderer::PipelineStateTag>  s_worldPso;  // WorldSpace: 深度テストあり (DEPTH_READ)
renderer::ResourceHandle<renderer::TextureTag>        s_whiteTexture;
renderer::ResourceHandle<renderer::TextureTag>        s_fontAtlas;
renderer::ResourceHandle<renderer::BufferTag>         s_imageVB;
renderer::ResourceHandle<renderer::BufferTag>         s_textVB;

// Fixed VB capacities
static constexpr uint32_t kImageVBVertices = 6;
static constexpr uint32_t kTextVBVertices  = 4096; // up to ~682 glyphs

// Transform から UI 矩形を取得する
// localPosition.xy = 左上座標 (キャンバス空間, Y↓)
// localScale.xy    = 幅・高さ (px)
struct Rect { math::Vector2 pos; math::Vector2 size; };
Rect RectFromTransform(const scene::Transform& t)
{
    return { { t.localPosition.x, t.localPosition.y },
             { t.localScale.x,    t.localScale.y    } };
}

math::Vector4 Multiply(const math::Vector4& a, const math::Vector4& b)
{
    return { a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w };
}

Glyph GetGlyph(char c)
{
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(c)))) {
    case 'A': return { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 };
    case 'B': return { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E };
    case 'C': return { 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F };
    case 'D': return { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E };
    case 'E': return { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F };
    case 'F': return { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 };
    case 'G': return { 0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F };
    case 'H': return { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 };
    case 'I': return { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F };
    case 'J': return { 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E };
    case 'K': return { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 };
    case 'L': return { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F };
    case 'M': return { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 };
    case 'N': return { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 };
    case 'O': return { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E };
    case 'P': return { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 };
    case 'Q': return { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D };
    case 'R': return { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 };
    case 'S': return { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E };
    case 'T': return { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 };
    case 'U': return { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E };
    case 'V': return { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 };
    case 'W': return { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A };
    case 'X': return { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 };
    case 'Y': return { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 };
    case 'Z': return { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F };
    case '0': return { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E };
    case '1': return { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E };
    case '2': return { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F };
    case '3': return { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E };
    case '4': return { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 };
    case '5': return { 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E };
    case '6': return { 0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E };
    case '7': return { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 };
    case '8': return { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E };
    case '9': return { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E };
    case '-': return { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 };
    case ':': return { 0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00 };
    default:  return { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    }
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

    // Persistent vertex buffers — allocated once, updated each use.
    s_imageVB = resources.CreateVertexBuffer(nullptr, kImageVBVertices * sizeof(UIVertex), sizeof(UIVertex));
    s_textVB  = resources.CreateVertexBuffer(nullptr, kTextVBVertices  * sizeof(UIVertex), sizeof(UIVertex));

    // Build SDF font atlas from built-in 5x7 bitmap glyphs.
    auto atlasData = BuildFontAtlas();
    s_fontAtlas = resources.CreateTexture(atlasData.data(),
                                          static_cast<uint32_t>(kAtlasW),
                                          static_cast<uint32_t>(kAtlasH));

    if (!s_shader.IsValid() || !s_textShader.IsValid() || !s_constants.IsValid() ||
        !s_pso.IsValid() || !s_worldPso.IsValid() || !s_whiteTexture.IsValid() ||
        !s_imageVB.IsValid() || !s_textVB.IsValid() || !s_fontAtlas.IsValid()) {
        FBZZ_LOG_ERROR("UISystem init failed: shader=%d textShader=%d cb=%d pso=%d worldPso=%d white=%d imageVB=%d textVB=%d atlas=%d",
                       static_cast<int>(s_shader.IsValid()),
                       static_cast<int>(s_textShader.IsValid()),
                       static_cast<int>(s_constants.IsValid()),
                       static_cast<int>(s_pso.IsValid()),
                       static_cast<int>(s_worldPso.IsValid()),
                       static_cast<int>(s_whiteTexture.IsValid()),
                       static_cast<int>(s_imageVB.IsValid()),
                       static_cast<int>(s_textVB.IsValid()),
                       static_cast<int>(s_fontAtlas.IsValid()));
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

CanvasRuntimeState BuildCanvasRuntimeState(const UICanvas& canvas,
                                           const GameObject& canvasGO,
                                           float viewportWidth,
                                           float viewportHeight,
                                           math::Vector2 rawMouseInViewport,
                                           const math::Matrix4& viewProjection,
                                           UIRenderTargetView targetView)
{
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

    float scale = ResolveCanvasScale(canvas, viewportWidth, viewportHeight);
    float visibleCanvasW = (std::max)(1.0f, viewportWidth) / scale;
    float visibleCanvasH = (std::max)(1.0f, viewportHeight) / scale;
    if (targetView == UIRenderTargetView::CanvasEditor) {
        visibleCanvasW = (std::max)(1.0f, canvas.canvasWidth);
        visibleCanvasH = (std::max)(1.0f, canvas.canvasHeight);
        scale = (std::max)(1.0f, viewportWidth) / visibleCanvasW;
    }

    state.canvasToClip = math::Matrix4::Orthographic(
        0.0f, visibleCanvasW,
        visibleCanvasH, 0.0f,
        0.0f, 1.0f);
    state.mouseInCanvasSpace = { rawMouseInViewport.x / scale, rawMouseInViewport.y / scale };
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

void SubmitText(renderer::IRenderer& renderer,
                renderer::ResourceManager& resources,
                const math::Matrix4& canvasToClip,
                renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                renderer::RenderLayer layer,
                const UIText& text,
                math::Vector2 position)
{
    if (!text.enabled || text.text.empty() || text.fontSize <= 0.0f) return;

    const float glyphH       = text.fontSize;
    const float glyphW       = glyphH * 5.0f / 7.0f;
    const float glyphAdvance = glyphW + text.letterSpacing;
    math::Vector2 pen = position;

    std::vector<UIVertex> verts;
    verts.reserve(text.text.size() * 6);

    for (char c : text.text) {
        if (c == '\n') {
            pen.x = position.x;
            pen.y += glyphH * 1.2f;
            continue;
        }
        if (c < kFirstChar || c > kLastChar) {
            pen.x += glyphAdvance;
            continue;
        }
        const GlyphUV& uv = s_glyphUVs[c - kFirstChar];
        const float x  = pen.x;
        const float y  = pen.y;
        const float x2 = x + glyphW;
        const float y2 = y + glyphH;
        verts.push_back({ {x,  y},  { uv.uvMin.x, uv.uvMin.y } });
        verts.push_back({ {x,  y2}, { uv.uvMin.x, uv.uvMax.y } });
        verts.push_back({ {x2, y},  { uv.uvMax.x, uv.uvMin.y } });
        verts.push_back({ {x2, y},  { uv.uvMax.x, uv.uvMin.y } });
        verts.push_back({ {x,  y2}, { uv.uvMin.x, uv.uvMax.y } });
        verts.push_back({ {x2, y2}, { uv.uvMax.x, uv.uvMax.y } });
        pen.x += glyphAdvance;
    }

    if (verts.empty()) return;

    const uint32_t vertCount = static_cast<uint32_t>(verts.size());
    if (vertCount > kTextVBVertices) {
        FBZZ_LOG_ERROR("UISystem: text too long (%u verts > %u capacity)", vertCount, kTextVBVertices);
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
    call.shader             = s_textShader.IsValid() ? s_textShader : s_shader;
    call.pipelineState      = pso;
    call.constantBuffers[0] = s_constants;
    call.vertexCount        = vertCount;
    call.layer              = layer;
    call.topology           = renderer::PrimitiveTopology::TRIANGLE_LIST;
    call.textures[0]        = s_fontAtlas.IsValid() ? s_fontAtlas : s_whiteTexture;
    renderer.Submit(call, resources);
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
            t.localPosition.x = cursor;
            t.localPosition.y = layout.paddingTop;
            cursor += t.localScale.x + layout.spacing;
        } else {
            t.localPosition.x = layout.paddingLeft;
            t.localPosition.y = cursor;
            cursor += t.localScale.y + layout.spacing;
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

void ProcessUIEventsRecursive(GameObject& go, math::Vector2 mouseInCanvasSpace, bool mousePressed)
{
    if (!go.activeSelf()) return;

    auto* image  = go.GetComponent<UIImage>();
    auto* button = go.GetComponent<UIButton>();
    if (button) {
        if (image && image->enabled)
            UpdateButton(*button, RectFromTransform(go.transform), mouseInCanvasSpace, mousePressed);
        else {
            button->onClick = false;
            button->onEnter = false;
            button->onExit  = false;
            button->state   = UIButtonState::NORMAL;
        }
    }

    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i))
            ProcessUIEventsRecursive(*child, mouseInCanvasSpace, mousePressed);
    }
}

// canvasToClip: SubmitImage / SubmitText へそのまま渡す変換行列。
// pso / layer:  ScreenSpace と WorldSpace で異なる PSO とレイヤーを切り替える。
void RenderCanvasRecursive(GameObject& go,
                           renderer::IRenderer& renderer,
                           renderer::ResourceManager& resources,
                           const math::Matrix4& canvasToClip,
                           renderer::ResourceHandle<renderer::PipelineStateTag> pso,
                           renderer::RenderLayer layer)
{
    if (!go.activeSelf()) return;

    auto* image  = go.GetComponent<UIImage>();
    auto* button = go.GetComponent<UIButton>();
    auto* text   = go.GetComponent<UIText>();

    if (image && image->enabled) {
        if (!image->texturePath.empty())
            image->texture = resources.LoadTexture(image->texturePath);

        const Rect    r     = RectFromTransform(go.transform);
        math::Vector4 color = image->color;
        if (button)
            color = Multiply(color, ButtonTint(*button));
        const auto& qr = go.transform.localRotation;
        const float zAngle = std::atan2f(2.0f*(qr.w*qr.z + qr.x*qr.y),
                                          1.0f - 2.0f*(qr.y*qr.y + qr.z*qr.z));
        SubmitImage(renderer, resources, canvasToClip, pso, layer,
                    r.pos, r.size, color,
                    image->uvMin, image->uvMax,
                    image->texture.IsValid() ? image->texture : s_whiteTexture,
                    zAngle);
    }

    if (text && text->enabled) {
        const math::Vector2 pos = { go.transform.localPosition.x, go.transform.localPosition.y };
        SubmitText(renderer, resources, canvasToClip, pso, layer, *text, pos);
    }

    for (int i = 0; i < go.GetChildCount(); ++i) {
        if (GameObject* child = go.GetChild(i))
            RenderCanvasRecursive(*child, renderer, resources, canvasToClip, pso, layer);
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
        ProcessUIEventsRecursive(*entry.go, state.mouseInCanvasSpace, mousePressed);
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
        RenderCanvasRecursive(*entry.go, renderer, resources, state.canvasToClip, state.pso, state.layer);
    }
}

} // namespace

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
