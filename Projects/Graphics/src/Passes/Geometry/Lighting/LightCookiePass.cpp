/// @file    LightCookiePass.cpp
/// @brief   ライト Cookie の元テクスチャをアトラスのタイルへ焼き直す
/// @author  Hasegawa Jin
/// @date    2026-08-25
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>

#include "Graphics/Renderer/Commands/DrawCall.hpp"
#include "Graphics/Renderer/Resources/ResourceManager.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace fbzz::renderer {

namespace {

/// @note スロットごとに前回焼いた内容。これと一致していればタイルはそのまま使える。
/// @note ShadowPass と同じく、レンダースレッド 1 本からしか触られない。
struct BakedCookie {
    std::string path;
    ResourceHandle<TextureTag> texture;
    bool srgb = false;
    float       rotationRad = 0.0f;
    bool        valid       = false;
};
std::array<BakedCookie, kMaxLightCookies> s_baked;

} // namespace

void ReleaseLightCookieCache()
{
    for (auto& baked : s_baked) {
        baked = BakedCookie{};
    }
}

/// @note 前回焼いた内容 (`s_baked`) と一致するスロットは焼き直さない。ライトの追加・削除や
/// @note Inspector でのパス・回転変更だけが再焼き対象になる。
void ExecuteLightCookiePass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    const int count = std::clamp(ctx.lightCookieViewCount, 0, kMaxLightCookies);

    // 使わなくなったスロットの記録は捨てる。同じスロットが別の Cookie へ割り当て
    // 直されたとき、たまたま前回と同じパスでも「焼き直し済み」と誤判定しないため。
    for (int i = count; i < kMaxLightCookies; ++i) {
        s_baked[i] = BakedCookie{};
    }

    if (count == 0) {
        return;
    }
    if (!ctx.Res().Target("LightCookieAtlas").IsValid() || !h.cookieBlitShader.IsValid()
        || !h.cookieBlitCB.IsValid() || !h.cookieBlitPSO.IsValid()) {
        return;
    }

    // 焼き直しが要るスロットだけ集める。1 枚も無ければ RT を触らずに帰る。
    bool anyDirty = false;
    for (int i = 0; i < count; ++i) {
        const LightCookieView& view = ctx.lightCookieViews[i];
        const BakedCookie&     baked = s_baked[i];
        if (!baked.valid || baked.path != view.sourcePath || baked.texture != view.sourceTexture || baked.srgb != view.srgb
            || std::abs(baked.rotationRad - view.rotationRad) > 1.0e-4f) {
            anyDirty = true;
            break;
        }
    }
    if (!anyDirty) {
        return;
    }

    renderer.SetRenderTarget(ctx.Res().Target("LightCookieAtlas"), resources);
    // クリアは白 (素通り)。Cookie は乗算マスクなので黒だとライトが完全に消える。
    // 焼けなかったタイルは「効かないだけ」に留める。
    renderer.Clear({ 1.0f, 1.0f, 1.0f, 1.0f });
    // クリアは全面へ 1 回。SetRenderTarget はビューポートを RT 全体へ戻すため、
    // タイルごとのビューポート設定は必ずこの後に行う。

    for (int i = 0; i < count; ++i) {
        const LightCookieView& view = ctx.lightCookieViews[i];
        if (view.sourcePath.empty()) {
            continue;
        }

        // 焼いたアトラスが絵を持つので、元テクスチャは焼く瞬間だけ要る。利用権は使われなくなれば手放される。
        const auto sourceTex = view.sourceTexture;
        if (!sourceTex.IsValid()) {
            // ロードできなかったスロットは白のまま残し、次フレームで再挑戦させる。
            s_baked[i] = BakedCookie{};
            continue;
        }

        renderer.SetViewport(view.viewportX, view.viewportY,
                             kLightCookieTileSize, kLightCookieTileSize);

        CookieBlitCB blitData{};
        blitData.cookieRotation = view.rotationRad;
        blitData.cookieSrgb     = view.srgb ? 1u : 0u;
        resources.Update(h.cookieBlitCB, &blitData, sizeof(CookieBlitCB));

        renderer::DrawCall dc;
        dc.shader             = h.cookieBlitShader;
        dc.pipelineState      = h.cookieBlitPSO;
        dc.constantBuffers[cb::MATERIAL] = h.cookieBlitCB;
        dc.textures[srv::ALBEDO]        = sourceTex;
        // 頂点バッファ無しの全画面三角形
        dc.vertexCount        = 3;
        renderer.Submit(dc, resources);

        s_baked[i].texture = view.sourceTexture;
        s_baked[i].srgb = view.srgb;
        s_baked[i].path        = view.sourcePath;
        s_baked[i].rotationRad = view.rotationRad;
        s_baked[i].valid       = true;
    }

    // 後続パスがビューポートを RT 全体だと仮定してよいよう、タイル絞りを解除する。
    renderer.SetViewport(0, 0, kLightCookieAtlasWidth, kLightCookieAtlasHeight);
}


void LightCookiePass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Write("LightCookieAtlas");
}

void LightCookiePass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteLightCookiePass(ctx);
}
} // namespace fbzz::renderer
