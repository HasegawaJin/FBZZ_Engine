// FBZZ Engine
// VolumetricLightPass.cpp | fbzz::scene
// 体積光（ゴッドレイ・光柱）Compute パス。
// WHY: レイマーチで各ピクセルからカメラ → シーンまでのパスを積分し、
//      シャドウマップで遮蔽判定しながら Mie 散乱を蓄積する。
//      結果は HDR バッファに加算合成し、霧の中の光差し込みを表現する。
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/ComputeCall.hpp>

namespace fbzz::scene {

void ExecuteVolumetricLightPass(RenderPassContext& ctx)
{
    auto& r           = ctx.renderer;
    auto& resources   = ctx.resources;
    auto& h           = ctx.handles;
    const auto& vol   = ctx.settings.volumetricLight;

    if (!vol.enabled ||
        !h.volumetricShader.IsValid() || !h.volumetricResult.IsValid() ||
        !h.shadowMapRT.IsValid())
        return;

    // WHAT: 深度バッファ(t7)とシャドウマップ(t8)をサンプルしてレイマーチ。
    //       Henyey-Greenstein 散乱で前方散乱を計算し UAV_VOLUMETRIC(u4) に出力する。
    renderer::ComputeCall volDC;
    volDC.shader             = h.volumetricShader;
    volDC.constantBuffers[0] = h.frameCB;             // b0: CameraConstants
    volDC.constantBuffers[3] = h.lightCB;             // b3: LightConstants (lightDir, lightColor)
    volDC.constantBuffers[4] = h.shadowCB;            // b4: ShadowConstants (lightVP, bias)
    volDC.constantBuffers[8] = h.advancedGraphicsCB;  // b8: volSteps, volScattering, volMaxDist
    // t7: シーン深度。レイ終端に使うため、Terrain/Detail/Foliage を含む完全な不透明深度が要る。
    // WHY: それらは Deferred でも hdrRT の depth へ描かれる。GBuffer depth には地形が無いため、
    //      そこを読むとゴッドレイが地形を貫通して手前に漏れる（雲と同じ不具合）。常に hdrRT を読む。
    volDC.srvInputs[7]       = resources.GetDepthTexture(h.hdrRT);
    volDC.srvInputs[8]       = resources.GetDepthTexture(h.shadowMapRT); // t8: Shadow map
    volDC.uavOutputs[4]      = h.volumetricResult;    // u4: UAV_VOLUMETRIC
    volDC.dispatchX          = (ctx.width  + 7) / 8;
    volDC.dispatchY          = (ctx.height + 7) / 8;
    volDC.dispatchZ          = 1;
    r.Dispatch(volDC, resources);
}

} // namespace fbzz::scene
