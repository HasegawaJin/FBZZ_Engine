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
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/RenderState.hpp>

namespace fbzz::scene {

void ExecuteVolumetricLightPass(RenderPassContext& ctx)
{
    auto& r           = ctx.renderer;
    auto& resources   = ctx.resources;
    auto& h           = ctx.handles;
    const auto& vol   = ctx.settings.volumetricLight;

    if (!vol.enabled ||
        !h.volumetricShader.IsValid() || !h.volumetricResult.IsValid() ||
        !ctx.Res().Target("ShadowMap").IsValid())
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
    volDC.srvInputs[7]       = resources.GetDepthTexture(ctx.Res().Target("HDR"));
    volDC.srvInputs[8]       = resources.GetDepthTexture(ctx.Res().Target("ShadowMap")); // t8: Shadow map
    if (cloudActive)
        volDC.srvInputs[26]  = h.cloudShapeTex;       // t26: TEX_CLOUD_SHAPE
    volDC.uavOutputs[4]      = h.volumetricResult;    // u4: UAV_VOLUMETRIC
    volDC.dispatchX          = (ctx.width  + 7) / 8;
    volDC.dispatchY          = (ctx.height + 7) / 8;
    volDC.dispatchZ          = 1;
    r.Dispatch(volDC, resources);

    // 積分結果を HDR へ加算する。
    //
    // WHY Composite ではなくここで足すか: Composite は水も半透明も描き終わった後に走る。
    //     レイは不透明深度で止まっているので、そこで足すと «水底までの光芒» が水面の
    //     手前に描かれる。不透明しか無いこの時点で足しておけば、後続の透明描画が
    //     普通に上へ乗り、遮蔽が正しくなる。
    // NOTE: CS 出力は既に volLightIntensity を掛けてある。ここで再度掛けないこと。
    static uint64_t s_resetVersion = resources.GetResetVersion();
    static auto copyShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
    // 加算合成の全画面三角形。深度は見ない (レイマーチ側が深度で終端を決めている)。
    static auto additivePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::ADDITIVE,
        renderer::DepthMode::DEPTH_OFF
    });
    if (s_resetVersion != resources.GetResetVersion()) {
        s_resetVersion = resources.GetResetVersion();
        copyShader     = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
        additivePSO    = resources.CreatePipelineState({
            renderer::RasterizerMode::SOLID,
            renderer::BlendMode::ADDITIVE,
            renderer::DepthMode::DEPTH_OFF
        });
    }
    if (!copyShader.IsValid() || !additivePSO.IsValid())
        return;

    // Dispatch は OM の RTV/DSV を外すので、描く前に張り直す。
    r.SetRenderTarget(ctx.Res().Target("HDR"), resources);

    renderer::DrawCall applyDC;
    applyDC.shader        = copyShader;
    applyDC.pipelineState = additivePSO;
    applyDC.vertexCount   = 3;
    applyDC.textures[5]   = h.volumetricResult; // TEX_GBUFFER0: CopyColor の入力スロット
    r.Submit(applyDC, resources);
}


void VolumetricLightPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("ShadowMap").ReadWrite("HDR").Write("VolumetricResult");
}

bool VolumetricLightPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.volumetricLight.enabled;
}

void VolumetricLightPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteVolumetricLightPass(ctx);
}
} // namespace fbzz::scene
