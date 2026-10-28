/// @file    VolumetricLightPass.cpp
/// @brief   体積光（ゴッドレイ・光柱）Compute パス。
/// @author  Hasegawa Jin
/// @date    2026-06-23

/// @note レイマーチで各ピクセルのカメラ→シーンのパスをシャドウマップで遮蔽判定しつつ Mie 散乱で積分し、
/// @note HDR バッファへ加算合成する。
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/RenderState.hpp>
#include "UnderwaterPassHelpers.hpp"
#include <algorithm>

namespace fbzz::renderer {

void ExecuteVolumetricLightPass(RenderPassContext& ctx)
{
    ctx.underwaterLightPassActive = false;
    auto& r           = ctx.renderer;
    auto& resources   = ctx.resources;
    auto& h           = ctx.handles;
    const auto& vol   = ctx.settings.volumetricLight;
    if (ctx.rayPathViewActive || !ctx.settings.IsVolumetricLightActive()) return;
    const auto raw    = ctx.Res().Texture("VolumetricRaw");
    const bool sharedPlanned = ctx.renderScene && HasOpticalWaterMedium(*ctx.renderScene);
    const bool underwater = sharedPlanned || UsesOpticalUnderwater(ctx.environment.underwater);
    const bool sharedLight = sharedPlanned && HasOpticalWaterMedium(*ctx.renderScene, true);

    if ((underwater && ((sharedPlanned ? !sharedLight : ctx.environment.underwater.lightStrength <= 0.0f)
                || !ctx.waterMediumPassActive || !ctx.waterInterfacePassActive)) ||
        !h.volumetricShader.IsValid() || !h.volumetricResult.IsValid() ||
        !raw.IsValid() || ctx.width == 0u || ctx.height == 0u ||
        (!underwater && !ctx.Res().Target("ShadowMap").IsValid()))
        return;

    static uint64_t s_resourceOwner = 0;
    static uint64_t s_resetVersion = 0;
    static ResourceHandle<ShaderTag> upsampleShader, copyShader;
    static ResourceHandle<PipelineStateTag> additivePSO;
    if (s_resourceOwner != resources.GetInstanceId() || s_resetVersion != resources.GetResetVersion()) {
        s_resourceOwner = resources.GetInstanceId();
        s_resetVersion = resources.GetResetVersion();
        upsampleShader = resources.LoadShader("Assets/Shaders/PostProcess/Lighting/VolumetricLightUpsample.cs.hlsl");
        copyShader = resources.LoadShader("Assets/Shaders/PostProcess/Color/CopyColor.hlsl");
        additivePSO = resources.CreatePipelineState({
            renderer::RasterizerMode::SOLID,
            renderer::BlendMode::ADDITIVE,
            renderer::DepthMode::DEPTH_OFF
        });
    }
    if (!upsampleShader.IsValid() || !copyShader.IsValid() || !additivePSO.IsValid())
        return;

    PostProcCB waterData{};
    FillUnderwaterConstants(waterData, ctx.environment.underwater);
    if (sharedPlanned) {
        waterData.underwaterOpticalMode = 1.0f;
        waterData.underwaterLightStrength = 1.0f;
        waterData.underwaterMaxDistance = 0.0f;
        waterData.underwaterLightSteps = 16.0f;
        for (const auto& visibleBody : h.waterMediumBodies) {
            if (visibleBody.waterInputIndex >= ctx.renderScene->water.size()) continue;
            const auto& body = ctx.renderScene->water[visibleBody.waterInputIndex];
            if (!body.medium.opticalEnabled) continue;
            waterData.underwaterMaxDistance = (std::max)(waterData.underwaterMaxDistance, body.medium.maxDistance);
            waterData.underwaterLightSteps = (std::max)(waterData.underwaterLightSteps, static_cast<float>(body.medium.lightSteps));
        }
    }
    /// @note 材質の光学距離・ステップは媒体側の上限。PostProcess の品質予算と強度を迂回しない。
    waterData.underwaterMaxDistance = (std::min)(waterData.underwaterMaxDistance, vol.maxDist);
    waterData.underwaterLightSteps = (std::min)(waterData.underwaterLightSteps,
        static_cast<float>(std::clamp(vol.steps, 16, 64)));
    waterData.underwaterLightStrength *= vol.intensity;
    waterData.time = ctx.time;
    resources.Update(h.postprocCB, &waterData, sizeof(waterData));

    /// @note 雲の切れ間から差す光の線は、シャドウマップに写らない頭上の雲で光芒を遮って作る。雲は画面空間
    /// @note レイマーチで描くためシャドウマップに入らず、同じ密度場 (CloudVolume.hlsli) を引かないと
    /// @note 光芒は一様な靄のままで «雲の隙間» の形が出ない。
    const bool cloudActive = UpdateVolumetricCloudConstants(ctx);

    /// @note 深度バッファ(t7)とシャドウマップ(t8)をサンプルしてレイマーチ。Henyey-Greenstein 散乱で
    /// @note 積分解像度は実リソースから求める。モード切替と奇数寸法でも全画素を記録する。
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
    volDC.constantBuffers[5] = h.postprocCB;
    /// @note b8: volSteps, volScattering, volMaxDist
    volDC.constantBuffers[8] = h.advancedGraphicsCB;
    /// @note Reuse the current view's existing light list and per-sample cluster Z; water volume never builds another Scene light scan.
    if (ctx.clusterLightMode != ClusterLightMode::Legacy) {
        volDC.constantBuffers[9] = h.clusterCB;
        volDC.srvBuffers[29] = h.punctualLightBuffer;
        if (UsesClusterLightIndices(ctx.clusterLightMode))
            volDC.srvBuffers[30] = h.clusterIndexBuffer;
    }
    if (h.punctualShadowCB.IsValid()) {
        volDC.constantBuffers[12] = h.punctualShadowCB;
        volDC.srvInputs[28] = resources.GetDepthTexture(ctx.Res().Target("PunctualShadowMap"));
        volDC.srvInputs[31] = resources.GetColorTexture(ctx.Res().Target("LightCookieAtlas"), 0);
    }
    if (ctx.waterMediumPassActive && ctx.waterInterfacePassActive) {
        volDC.srvBuffers[14] = h.waterMediumTable;
        volDC.srvInputs[15] = resources.GetColorTexture(ctx.Res().Target("WaterMediumAtlas"), 0);
        volDC.srvInputs[13] = resources.GetColorTexture(ctx.Res().Target("WaterInterface"), 0);
    }
    /// @note t7: シーン深度。レイ終端に Terrain を含む完全な不透明深度が要る。Terrain は Deferred でも
    /// @note hdrRT の depth へ描かれ GBuffer depth には無いため、GBuffer を読むとゴッドレイが地形を
    /// @note 貫通して手前に漏れる (雲と同じ不具合)。常に hdrRT を読む。
    volDC.srvInputs[7]       = resources.GetDepthTexture(ctx.Res().Target("HDR"));
    /// @note t8: Shadow map
    volDC.srvInputs[8]       = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    if (cloudActive)
        /// @note t26: TEX_CLOUD_SHAPE
        volDC.srvInputs[26]  = h.cloudShapeTex;
    /// @note u4: UAV_VOLUMETRIC
    volDC.uavOutputs[4]      = raw;
    const auto* rawTexture = resources.Get(raw);
    if (!rawTexture) return;
    volDC.dispatchX          = (rawTexture->GetWidth() + 7u) / 8u;
    volDC.dispatchY          = (rawTexture->GetHeight() + 7u) / 8u;
    volDC.dispatchZ          = 1;
    for (const auto slot : {0u, 3u, 4u, 5u, 8u})
        if (!resources.Get(volDC.constantBuffers[slot])) return;
    if (!resources.Get(raw) || !resources.Get(h.volumetricResult)
        || !resources.Get(volDC.srvInputs[7]) || (!underwater && !resources.Get(volDC.srvInputs[8]))) return;
    if (underwater && (!resources.Get(volDC.srvBuffers[14])
        || !resources.Get(volDC.srvInputs[15]) || !resources.Get(volDC.srvInputs[13]))) return;
    /// @note HDR の DSV と同じ深度を読むため、再構成は描画先を束縛しない Compute で行う。
    /// @note 深度差の大きい隣接画素を除き、手前の地形へ奥の光芒がにじむのを抑える。
    /// @see https://developer.download.nvidia.com/assets/gameworks/papers/Fast_Flexible_Physically-Based_Volumetric_Light_Scattering.pdf Apply Lighting / Composite Results
    renderer::ComputeCall upsampleDC;
    upsampleDC.shader = upsampleShader;
    upsampleDC.constantBuffers[0] = h.frameCB;
    upsampleDC.srvInputs[5] = raw;
    upsampleDC.srvInputs[7] = resources.GetDepthTexture(ctx.Res().Target("HDR"));
    upsampleDC.uavOutputs[4] = h.volumetricResult;
    upsampleDC.dispatchX = (ctx.width + 7u) / 8u;
    upsampleDC.dispatchY = (ctx.height + 7u) / 8u;
    upsampleDC.dispatchZ = 1;
    if (r.GetCapabilities().bindless) {
        /// @note 両段の実記録を一回の引渡しで確認する。下段失敗でも初段はrollbackせず、合成へ公開しない。
        /// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/threaded-rendering-in-unreal-engine Inter-thread communication.
        bool dispatchesRecorded = false;
        /// @note Both dispatches share one synchronous render-resource handoff on threaded backends.
        r.ExecuteResourceOperation([&] {
            dispatchesRecorded = r.TryDispatch(volDC, resources)
                && r.TryDispatch(upsampleDC, resources);
        });
        if (!dispatchesRecorded) return;
    } else {
        r.Dispatch(volDC, resources);
        r.Dispatch(upsampleDC, resources);
    }
    if (underwater) {
        /// @note 水中はCompositeが背景にBeer透過を掛けた後へ散乱を合成し、二重減衰を避ける。
        ctx.underwaterLightPassActive = true;
        return;
    }

    /// @note 積分結果を HDR へ加算する。Composite でなくここで足す理由: レイは不透明深度で止まっており、
    /// @note Composite (半透明描画後) まで待つと «水底までの光芒» が水面の手前に描かれる。ここで足せば
    /// @note 後続の透明描画が普通に上へ乗り遮蔽が正しくなる。
    /// @note CS 出力は既に volLightIntensity を掛けてある。ここで再度掛けないこと。
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


void VolumetricLightPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    builder.Read("ShadowMap").Write("VolumetricRaw").Write("VolumetricResult");
    if (ctx.resourceRegistry.Target("PunctualShadowMap").IsValid())
        builder.Read("PunctualShadowMap");
    if (ctx.resourceRegistry.Target("LightCookieAtlas").IsValid())
        builder.Read("LightCookieAtlas");
    if (ctx.resourceRegistry.Target("WaterMediumAtlas").IsValid())
        builder.Read("WaterMediumAtlas");
    if (ctx.resourceRegistry.Target("WaterInterface").IsValid())
        builder.Read("WaterInterface");
    if ((ctx.renderScene && HasOpticalWaterMedium(*ctx.renderScene))
        || UsesOpticalUnderwater(ctx.environment.underwater)) builder.Read("HDR");
    else builder.ReadWrite("HDR");
}

bool VolumetricLightPass::IsEnabled(const RenderPassContext& ctx) const
{
    if (ctx.rayPathViewActive || !ctx.settings.IsVolumetricLightActive()) return false;
    const auto& water = ctx.environment.underwater;
    if (ctx.renderScene && HasOpticalWaterMedium(*ctx.renderScene)) return HasOpticalWaterMedium(*ctx.renderScene, true);
    return UsesOpticalUnderwater(water) ? water.lightStrength > 0.0f : ctx.settings.volumetricLight.enabled;
}

void VolumetricLightPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteVolumetricLightPass(ctx);
}
} /// @note namespace fbzz::renderer
