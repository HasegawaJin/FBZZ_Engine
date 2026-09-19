/// @file    LightCookiePass.cpp
/// @brief   ライト Cookie の元テクスチャをアトラスのタイルへ焼き直す
/// @author  Hasegawa Jin
/// @date    2026-08-25
#include "GeometryPasses.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace fbzz::scene {

namespace {

/// スロットごとに前回焼いた内容。これと一致していればタイルはそのまま使える。
/// ShadowPass と同じく、レンダースレッド 1 本からしか触られない。
struct BakedCookie {
    std::string path;
    float       rotationRad = 0.0f;
    bool        valid       = false;
};
std::array<BakedCookie, kMaxLightCookies> s_baked;

} // namespace

void ReleaseLightCookieCache()
{
    for (auto& baked : s_baked) baked = BakedCookie{};
}

/// @note 前回焼いた内容 (`s_baked`) と一致するスロットは焼き直さない。ライトの追加・削除や
///       Inspector でのパス・回転変更だけが再焼き対象になる。
void ExecuteLightCookiePass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    const int count = std::clamp(ctx.lightCookieViewCount, 0, kMaxLightCookies);

    /// @note 使わなくなったスロットの記録は捨てる。同じスロットが別の Cookie へ割り当て
    ///       直されたとき、たまたま前回と同じパスでも「焼き直し済み」と誤判定しないため。
    for (int i = count; i < kMaxLightCookies; ++i) s_baked[i] = BakedCookie{};

    if (count == 0) return;
    if (!ctx.Res().Target("LightCookieAtlas").IsValid() || !h.cookieBlitShader.IsValid()
        || !h.cookieBlitCB.IsValid() || !h.cookieBlitPSO.IsValid()) {
        return;
    }

    /// @note 焼き直しが要るスロットだけ集める。1 枚も無ければ RT を触らずに帰る。
    bool anyDirty = false;
    for (int i = 0; i < count; ++i) {
        const LightCookieView& view = ctx.lightCookieViews[i];
        const BakedCookie&     baked = s_baked[i];
        if (!baked.valid || baked.path != view.sourcePath
            || std::abs(baked.rotationRad - view.rotationRad) > 1.0e-4f) {
            anyDirty = true;
            break;
        }
    }
    if (!anyDirty) return;

    renderer.SetRenderTarget(ctx.Res().Target("LightCookieAtlas"), resources);
    /// @note クリアは白 (素通り)。Cookie は乗算マスクなので黒だとライトが完全に消える。
    ///       焼けなかったタイルは「効かないだけ」に留める。
    renderer.Clear({ 1.0f, 1.0f, 1.0f, 1.0f });
    /// @note クリアは全面へ 1 回。SetRenderTarget はビューポートを RT 全体へ戻すため、
    ///       タイルごとのビューポート設定は必ずこの後に行う。

    for (int i = 0; i < count; ++i) {
        const LightCookieView& view = ctx.lightCookieViews[i];
        if (view.sourcePath.empty()) continue;

        const auto sourceTex = resources.LoadTexture(view.sourcePath);
        if (!sourceTex.IsValid()) {
            /// @note ロードできなかったスロットは白のまま残し、次フレームで再挑戦させる。
            s_baked[i] = BakedCookie{};
            continue;
        }

        renderer.SetViewport(view.viewportX, view.viewportY,
                             kLightCookieTileSize, kLightCookieTileSize);

        CookieBlitCB blitData{};
        blitData.cookieRotation = view.rotationRad;
        blitData.cookieSrgb     = IsEffectTextureSrgb(view.sourcePath) ? 1u : 0u;
        resources.Update(h.cookieBlitCB, &blitData, sizeof(CookieBlitCB));

        renderer::DrawCall dc;
        dc.shader             = h.cookieBlitShader;
        dc.pipelineState      = h.cookieBlitPSO;
        dc.constantBuffers[2] = h.cookieBlitCB;
        dc.textures[0]        = sourceTex;
        /// @note 頂点バッファ無しの全画面三角形
        dc.vertexCount        = 3;
        renderer.Submit(dc, resources);

        s_baked[i].path        = view.sourcePath;
        s_baked[i].rotationRad = view.rotationRad;
        s_baked[i].valid       = true;
    }

    /// @note 後続パスがビューポートを RT 全体だと仮定してよいよう、タイル絞りを解除する。
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
} // namespace fbzz::scene
