/// @file    VolumetricLightPass.cpp
/// @brief   体積光（ゴッドレイ・光柱）Compute パス。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// WHY: レイマーチで各ピクセルからカメラ → シーンまでのパスを積分し、
/// シャドウマップで遮蔽判定しながら Mie 散乱を蓄積する。
/// 結果は HDR バッファに加算合成し、霧の中の光差し込みを表現する。
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

    // 雲の切れ間から差す光の線は、シャドウマップに写らない頭上の雲で光芒を遮って作る。
    // WHY: 雲は画面空間レイマーチで描かれるためシャドウマップには一切入らない。
    //      同じ密度場 (CloudVolume.hlsli) をここでも引かないと、雲があっても光芒は
    //      一様な靄のままで「雲の隙間」の形が出ない。
    const bool cloudActive = UpdateVolumetricCloudConstants(ctx);

    // WHAT: 深度バッファ(t7)とシャドウマップ(t8)をサンプルしてレイマーチ。
    //       Henyey-Greenstein 散乱で前方散乱を計算し UAV_VOLUMETRIC(u4) に出力する。
    renderer::ComputeCall volDC;
    volDC.shader             = h.volumetricShader;
    volDC.constantBuffers[0] = h.frameCB;             // b0: CameraConstants
    volDC.constantBuffers[2] = h.volumetricCloudCB;   // b2: VolumetricCloudConstants
    volDC.constantBuffers[3] = h.lightCB;             // b3: LightConstants (lightDir, lightColor)
    volDC.constantBuffers[4] = h.shadowCB;            // b4: ShadowConstants (lightVP, bias)
    volDC.constantBuffers[8] = h.advancedGraphicsCB;  // b8: volSteps, volScattering, volMaxDist
    // t7: シーン深度。レイ終端に使うため、Terrain を含む完全な不透明深度が要る。
    // WHY: それらは Deferred でも hdrRT の depth へ描かれる。GBuffer depth には地形が無いため、
    //      そこを読むとゴッドレイが地形を貫通して手前に漏れる（雲と同じ不具合）。常に hdrRT を読む。
    volDC.srvInputs[7]       = resources.GetDepthTexture(h.hdrRT);
    volDC.srvInputs[8]       = resources.GetDepthTexture(h.shadowMapRT); // t8: Shadow map
    if (cloudActive)
        volDC.srvInputs[26]  = h.cloudShapeTex;       // t26: TEX_CLOUD_SHAPE
    volDC.uavOutputs[4]      = h.volumetricResult;    // u4: UAV_VOLUMETRIC
    volDC.dispatchX          = (ctx.width  + 7) / 8;
    volDC.dispatchY          = (ctx.height + 7) / 8;
    volDC.dispatchZ          = 1;
    r.Dispatch(volDC, resources);
}

} // namespace fbzz::scene
