// FBZZ Engine
// RenderPasses/SkyLightBakePass.cpp | fbzz::scene
// 空連動 IBL (環境システム設計 Phase A) の「②SkyLightBake」パス。
// SkyCapture が焼いた "SkyEnvCube" を irradiance / prefilter キューブマップへ畳み込み、
// EnvironmentResources に動的 IBL テクスチャとして保持する。
//
// WHY (既存 compute の再利用):
//   畳み込み自体は DX11IblBaker が editor の DDS ベイクで実証済み。本パスは抽象 IRenderer::BakeSkyLight
//   を呼ぶだけで、DX11 側がその実証済み Compute (IBL.IrradianceConvolution / IBL.PrefilteredEnvMap) を
//   キャプチャ済みキューブ SRV に対して走らせる。結果は ResourceManager::RegisterTexture で TextureTag 化する。
#include "GeometryPasses.hpp"
#include <Engine/Renderer/ITexture.hpp>
#include <cstdint>
#include <memory>

namespace fbzz::scene {

void ExecuteSkyLightBakePass(RenderPassContext& ctx)
{
    auto* env = ctx.environmentResources;
    if (!env || !env->needsConvolution)        return; // SkyCapture が焼き直したフレームのみ
    if (!ctx.handles.skyEnvCubeRT.IsValid())   return;

    // 畳み込み品質。irradiance は粗くて十分、prefilter は roughness を 5 mip に分割する。
    constexpr uint32_t kIrradianceSize = 32;
    constexpr uint32_t kPrefilterSize  = 128;
    constexpr uint32_t kPrefilterMips  = 5;
    constexpr uint32_t kSampleCount    = 128; // runtime 用に editor (1024) より控えめ

    std::unique_ptr<renderer::ITexture> irrTex, preTex;
    const bool ok = ctx.renderer.BakeSkyLight(
        ctx.handles.skyEnvCubeRT, ctx.resources,
        kIrradianceSize, kPrefilterSize, kPrefilterMips, kSampleCount,
        irrTex, preTex);

    if (ok && irrTex && preTex) {
        // 旧 IBL テクスチャを解放してから差し替える (dirty 毎に焼き直すためリークさせない)。
        if (env->skyIrradiance.IsValid()) ctx.resources.Release(env->skyIrradiance);
        if (env->skyPrefilter.IsValid())  ctx.resources.Release(env->skyPrefilter);
        env->skyIrradiance       = ctx.resources.RegisterTexture(std::move(irrTex));
        env->skyPrefilter        = ctx.resources.RegisterTexture(std::move(preTex));
        env->prefilteredMipCount = kPrefilterMips;
    }

    // 成否に関わらずフラグを消費する。失敗フレームを毎回リトライして固まらないようにし、
    // 次に SkyCapture が dirty を検知したときに再度焼き直す。
    env->needsConvolution = false;
}

} // namespace fbzz::scene
