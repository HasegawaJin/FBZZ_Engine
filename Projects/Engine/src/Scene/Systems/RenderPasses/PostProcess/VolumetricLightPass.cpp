/// @file    VolumetricLightPass.cpp
/// @brief   体積光（ゴッドレイ・光柱）Compute パス。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// @note レイマーチで各ピクセルのカメラ→シーンのパスをシャドウマップで遮蔽判定しつつ Mie 散乱で積分し、
///       HDR バッファへ加算合成する。
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

    /// @note 雲の切れ間から差す光の線は、シャドウマップに写らない頭上の雲で光芒を遮って作る。雲は画面空間
    ///       レイマーチで描くためシャドウマップに入らず、同じ密度場 (CloudVolume.hlsli) を引かないと
    ///       光芒は一様な靄のままで «雲の隙間» の形が出ない。
    const bool cloudActive = UpdateVolumetricCloudConstants(ctx);

    /// @note 深度バッファ(t7)とシャドウマップ(t8)をサンプルしてレイマーチ。Henyey-Greenstein 散乱で
    ///       前方散乱を計算し UAV_VOLUMETRIC(u4) に出力する。
    renderer::ComputeCall volDC;
    volDC.shader             = h.volumetricShader;
    /// @note b0: CameraConstants
    volDC.constantBuffers[0] = h.frameCB;
    /// @note b2: VolumetricCloudConstants
    volDC.constantBuffers[2] = h.volumetricCloudCB;
    /// @note b3: LightConstants (lightDir, lightColor)
    volDC.constantBuffers[3] = h.lightCB;
    /// @note b4: ShadowConstants (lightVP, bias)
    volDC.constantBuffers[4] = h.shadowCB;
    /// @note b8: volSteps, volScattering, volMaxDist
    volDC.constantBuffers[8] = h.advancedGraphicsCB;
    /// @note t7: シーン深度。レイ終端に Terrain を含む完全な不透明深度が要る。Terrain は Deferred でも
    ///       hdrRT の depth へ描かれ GBuffer depth には無いため、GBuffer を読むとゴッドレイが地形を
    ///       貫通して手前に漏れる (雲と同じ不具合)。常に hdrRT を読む。
    volDC.srvInputs[7]       = resources.GetDepthTexture(ctx.Res().Target("HDR"));
    /// @note t8: Shadow map
    volDC.srvInputs[8]       = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    if (cloudActive)
        /// @note t26: TEX_CLOUD_SHAPE
        volDC.srvInputs[26]  = h.cloudShapeTex;
    /// @note u4: UAV_VOLUMETRIC
    volDC.uavOutputs[4]      = h.volumetricResult;
    volDC.dispatchX          = (ctx.width  + 7) / 8;
    volDC.dispatchY          = (ctx.height + 7) / 8;
    volDC.dispatchZ          = 1;
    r.Dispatch(volDC, resources);

    /// @note 積分結果を HDR へ加算する。Composite でなくここで足す理由: レイは不透明深度で止まっており、
    ///       Composite (半透明描画後) まで待つと «水底までの光芒» が水面の手前に描かれる。ここで足せば
    ///       後続の透明描画が普通に上へ乗り遮蔽が正しくなる。
    /// @note CS 出力は既に volLightIntensity を掛けてある。ここで再度掛けないこと。
    static uint64_t s_resetVersion = resources.GetResetVersion();
    static auto copyShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
    /// @note 加算合成の全画面三角形。深度は見ない (レイマーチ側が深度で終端を決めている)。
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

    /// @note Dispatch は OM の RTV/DSV を外すので、描く前に張り直す。
    r.SetRenderTarget(ctx.Res().Target("HDR"), resources);

    renderer::DrawCall applyDC;
    applyDC.shader        = copyShader;
    applyDC.pipelineState = additivePSO;
    applyDC.vertexCount   = 3;
    /// @note TEX_GBUFFER0: CopyColor の入力スロット
    applyDC.textures[5]   = h.volumetricResult;
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
