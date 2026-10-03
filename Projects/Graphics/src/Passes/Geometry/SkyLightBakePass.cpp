/// @file    SkyLightBakePass.cpp
/// @brief   空連動 IBL (環境システム設計 Phase A) の「②SkyLightBake」パス。
/// @author  Hasegawa Jin
/// @date    2026-07-01

/// @note SkyCapture が焼いた "SkyEnvCube" を irradiance / prefilter キューブマップへ畳み込み、
/// @note EnvironmentResources に動的 IBL テクスチャとして保持する。

/// @note 畳み込み自体は DX11IblBaker が editor の DDS ベイクで実証済みのため、本パスは抽象
/// @note `IRenderer::BakeSkyLight` を呼ぶだけで、DX11 側がその Compute (IBL.IrradianceConvolution /
/// @note IBL.PrefilteredEnvMap) をキャプチャ済みキューブ SRV に対して走らせる。結果は
/// @note `ResourceManager::RegisterTexture` で TextureTag 化する。
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Renderer/ITexture.hpp>
#include <cstdint>
#include <memory>

namespace fbzz::renderer {

void ExecuteSkyLightBakePass(RenderPassContext& ctx)
{
    auto* env = ctx.environmentResources;
    /// @note SkyCapture が焼き直したフレームのみ
    if (!env || !env->needsConvolution)        return;
    if (!ctx.handles.skyEnvCubeRT.IsValid())   return;

    /// @note 畳み込み品質。irradiance は粗くて十分、prefilter は roughness を 5 mip に分割する。
    constexpr uint32_t kIrradianceSize = 32;
    constexpr uint32_t kPrefilterSize  = 128;
    constexpr uint32_t kPrefilterMips  = 5;
    /// @note runtime 用に editor (1024) より控えめ
    constexpr uint32_t kSampleCount    = 128;

    std::unique_ptr<renderer::ITexture> irrTex, preTex;
    const bool ok = ctx.renderer.BakeSkyLight(
        ctx.handles.skyEnvCubeRT, ctx.resources,
        kIrradianceSize, kPrefilterSize, kPrefilterMips, kSampleCount,
        irrTex, preTex);

    if (ok && irrTex && preTex) {
        /// @note 旧 IBL テクスチャを解放してから差し替える (dirty 毎に焼き直すためリークさせない)。
        if (env->skyIrradiance.IsValid()) ctx.resources.Release(env->skyIrradiance);
        if (env->skyPrefilter.IsValid())  ctx.resources.Release(env->skyPrefilter);
        env->skyIrradiance       = ctx.resources.RegisterTexture(std::move(irrTex));
        env->skyPrefilter        = ctx.resources.RegisterTexture(std::move(preTex));
        env->prefilteredMipCount = kPrefilterMips;
        env->immutableIblOwner = ctx.resources.Get(env->skyIrradiance) && ctx.resources.Get(env->skyPrefilter)
            ? &ctx.resources : nullptr;
        env->immutableIblEpoch = env->immutableIblOwner ? ctx.resources.GetResetVersion() : 0;
        env->immutableIrradiance = env->skyIrradiance;
        env->immutablePrefilter = env->skyPrefilter;
    }

    /// @note 成否に関わらずフラグを消費する。失敗フレームを毎回リトライして固まらないようにし、
    /// @note 次に SkyCapture が dirty を検知したときに再度焼き直す。
    env->needsConvolution = false;
    /// @note まだ一度も焼けていないのに失敗したら、次フレームの SkyCapture を強制 dirty にする。
    /// @note ConsumeDirty は SkyCapture の時点で署名を進めるため、ここで何もしないと太陽が 1.5°
    /// @note 動くか大気設定を触るまで再試行されず IBL が ambient 落ちのままになる。既に焼けた組が
    /// @note あるなら古い方を使い続ける方がましなので触らない。
    if (!ok && !env->HasBakedTextures())
        env->MarkDirty();
}

} /// @note namespace fbzz::renderer
