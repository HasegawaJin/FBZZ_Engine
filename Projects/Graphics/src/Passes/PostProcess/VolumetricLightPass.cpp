/// @file    VolumetricLightPass.cpp
/// @brief   体積光（ゴッドレイ・光柱）Compute パス。
/// @author  Hasegawa Jin
/// @date    2026-06-23

/// @note レイマーチで各ピクセルのカメラ→シーンのパスをシャドウマップで遮蔽判定しつつ Mie 散乱で積分し、
/// @note HDR バッファへ加算合成する。
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Pipeline/Passes/RenderPassContext.hpp>
#include <Graphics/Renderer/Commands/ComputeCall.hpp>
#include <Graphics/Renderer/Commands/DrawCall.hpp>
#include <Graphics/Renderer/State/RenderState.hpp>
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
    if (ctx.rayPathViewActive || !ctx.settings.IsVolumetricLightActive()) {
        return;
    }
    const auto raw    = ctx.Res().Texture("VolumetricRaw");
    const bool sharedPlanned = ctx.renderScene && HasOpticalWaterMedium(*ctx.renderScene);
    const bool submerged = UsesOpticalUnderwater(ctx.environment.underwater);
    const bool airLight = !submerged && ctx.settings.shadowEnabled && ctx.settings.IsPassEnabled("Shadow")
        && ctx.Res().Target("ShadowMap").IsValid();
    const bool waterLight = ctx.waterMediumPassActive && ctx.waterInterfacePassActive
        && (sharedPlanned ? HasOpticalWaterMedium(*ctx.renderScene, true)
                          : submerged && ctx.environment.underwater.lightStrength > 0.0f);

    if ((!airLight && !waterLight) || !h.volumetricShader.IsValid() || !h.volumetricResult.IsValid()
        || !raw.IsValid() || ctx.width == 0u || ctx.height == 0u) {
        return;
    }

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
    if (!upsampleShader.IsValid()) {
        return;
    }

    // 雲の切れ間から差す光の線は、シャドウマップに写らない頭上の雲で光芒を遮って作る。雲は画面空間
    // レイマーチで描くためシャドウマップに入らず、同じ密度場 (CloudVolume.hlsli) を引かないと
    // 光芒は一様な靄のままで «雲の隙間» の形が出ない。
    const bool cloudActive = UpdateVolumetricCloudConstants(ctx);

    // 深度バッファ(t7)とシャドウマップ(t8)をサンプルしてレイマーチ。Henyey-Greenstein 散乱で
    // 積分解像度は実リソースから求める。モード切替と奇数寸法でも全画素を記録する。
    renderer::ComputeCall volDC;
    volDC.shader             = h.volumetricShader;
    // b0: CameraConstants
    volDC.constantBuffers[cb::CAMERA] = h.frameCB;
    // b2: VolumetricCloudConstants
    volDC.constantBuffers[cb::VOLUMETRIC_CLOUD] = h.volumetricCloudCB;
    // b3: LightConstants (lightDir, lightColor)
    volDC.constantBuffers[cb::LIGHT] = h.lightCB;
    // b4: ShadowConstants (lightVP, bias)
    volDC.constantBuffers[cb::SHADOW] = h.shadowCB;
    volDC.constantBuffers[cb::POSTPROC] = h.postprocCB;
    // b8: volSteps, volScattering, volMaxDist
    volDC.constantBuffers[cb::ADVANCED_GRAPHICS] = h.advancedGraphicsCB;
    // Reuse the current view's existing light list and per-sample cluster Z; water volume never builds another Scene light scan.
    if (ctx.clusterLightMode != ClusterLightMode::Legacy) {
        volDC.constantBuffers[cb::CLUSTER] = h.clusterCB;
        volDC.srvBuffers[srv::PUNCTUAL_LIGHTS] = h.punctualLightBuffer;
        if (UsesClusterLightIndices(ctx.clusterLightMode)) {
            volDC.srvBuffers[srv::CLUSTER_INDICES] = h.clusterIndexBuffer;
        }
    }
    if (h.punctualShadowCB.IsValid()) {
        volDC.constantBuffers[cb::PUNCTUAL_SHADOW] = h.punctualShadowCB;
        volDC.srvInputs[srv::PUNCTUAL_SHADOW] = resources.GetDepthTexture(ctx.Res().Target("PunctualShadowMap"));
        volDC.srvInputs[srv::LIGHT_COOKIE] = resources.GetColorTexture(ctx.Res().Target("LightCookieAtlas"), 0);
    }
    if (ctx.waterMediumPassActive && ctx.waterInterfacePassActive) {
        volDC.srvBuffers[srv::WATER_MEDIUM_TABLE] = h.waterMediumTable;
        volDC.srvInputs[srv::WATER_MEDIUM_ATLAS] = resources.GetColorTexture(ctx.Res().Target("WaterMediumAtlas"), 0);
        volDC.srvInputs[srv::WATER_INTERFACE] = resources.GetColorTexture(ctx.Res().Target("WaterInterface"), 0);
    }
    // t7: シーン深度。レイ終端に Terrain を含む完全な不透明深度が要る。Terrain は Deferred でも
    // hdrRT の depth へ描かれ GBuffer depth には無いため、GBuffer を読むとゴッドレイが地形を
    // 貫通して手前に漏れる (雲と同じ不具合)。常に hdrRT を読む。
    volDC.srvInputs[srv::DEPTH]       = resources.GetDepthTexture(ctx.Res().Target("HDR"));
    // t8: Shadow map
    volDC.srvInputs[srv::SHADOW]       = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    if (cloudActive)
        // t26: TEX_CLOUD_SHAPE
        volDC.srvInputs[srv::CLOUD_SHAPE]  = h.cloudShapeTex;
    // u4: UAV_VOLUMETRIC
    volDC.uavOutputs[uav::VOLUMETRIC]      = raw;
    const auto* rawTexture = resources.Get(raw);
    if (!rawTexture) {
        return;
    }
    volDC.dispatchX          = (rawTexture->GetWidth() + 7u) / 8u;
    volDC.dispatchY          = (rawTexture->GetHeight() + 7u) / 8u;
    volDC.dispatchZ          = 1;
    for (const auto slot : {cb::CAMERA, cb::LIGHT, cb::SHADOW, cb::POSTPROC, cb::ADVANCED_GRAPHICS}) {
        if (!resources.Get(volDC.constantBuffers[slot])) {
            return;
        }
    }
    if (!resources.Get(raw) || !resources.Get(h.volumetricResult) || !resources.Get(volDC.srvInputs[srv::DEPTH])) {
        return;
    }
    // HDR の DSV と同じ深度を読むため、再構成は描画先を束縛しない Compute で行う。
    // 深度差の大きい隣接画素を除き、手前の地形へ奥の光芒がにじむのを抑える。
    /// @see https://developer.download.nvidia.com/assets/gameworks/papers/Fast_Flexible_Physically-Based_Volumetric_Light_Scattering.pdf Apply Lighting / Composite Results
    renderer::ComputeCall upsampleDC;
    upsampleDC.shader = upsampleShader;
    upsampleDC.constantBuffers[cb::CAMERA] = h.frameCB;
    upsampleDC.srvInputs[srv::VOLUMETRIC_RAW] = raw;
    upsampleDC.srvInputs[srv::DEPTH] = resources.GetDepthTexture(ctx.Res().Target("HDR"));
    upsampleDC.uavOutputs[uav::VOLUMETRIC] = h.volumetricResult;
    upsampleDC.dispatchX = (ctx.width + 7u) / 8u;
    upsampleDC.dispatchY = (ctx.height + 7u) / 8u;
    upsampleDC.dispatchZ = 1;
    const auto dispatchLight = [&](const PostProcCB& data) {
        resources.Update(h.postprocCB, &data, sizeof(data));
        if (!r.GetCapabilities().bindless) {
            r.Dispatch(volDC, resources);
            r.Dispatch(upsampleDC, resources);
            return true;
        }
        // 両段の実記録を一回の引渡しで確認する。下段失敗でも初段はrollbackせず、合成へ公開しない。
        /// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/threaded-rendering-in-unreal-engine Inter-thread communication.
        bool dispatchesRecorded = false;
        // Both dispatches share one synchronous render-resource handoff on threaded backends.
        r.ExecuteResourceOperation([&] {
            dispatchesRecorded = r.TryDispatch(volDC, resources)
                && r.TryDispatch(upsampleDC, resources);
        });
        return dispatchesRecorded;
    };

    if (airLight && copyShader.IsValid() && additivePSO.IsValid() && resources.Get(volDC.srvInputs[srv::SHADOW])) {
        PostProcCB airData{};
        airData.time = ctx.time;
        // Negative mode enables the first-water-interface distance cap in the air dispatch.
        if (sharedPlanned && resources.Get(volDC.srvInputs[srv::WATER_INTERFACE])) {
            airData.underwaterOpticalMode = -1.0f;
        }
        if (dispatchLight(airData)) {
            // 積分結果を HDR へ加算する。Composite でなくここで足す理由: レイは不透明深度で止まっており、
            // Composite (半透明描画後) まで待つと «水底までの光芒» が水面の手前に描かれる。ここで足せば
            // 後続の透明描画が普通に上へ乗り遮蔽が正しくなる。
            // CS 出力は既に volLightIntensity を掛けてある。ここで再度掛けないこと。
            // Dispatch は OM の RTV/DSV を外すので、描く前に張り直す。
            r.SetRenderTarget(ctx.Res().Target("HDR"), resources);

            renderer::DrawCall applyDC;
            applyDC.shader        = copyShader;
            applyDC.pipelineState = additivePSO;
            applyDC.vertexCount   = 3;
            // TEX_GBUFFER0: CopyColor の入力スロット
            applyDC.textures[srv::POST_COLOR]   = h.volumetricResult;
            r.Submit(applyDC, resources);
        }
    }

    if (!waterLight || !resources.Get(volDC.srvBuffers[srv::WATER_MEDIUM_TABLE])
        || !resources.Get(volDC.srvInputs[srv::WATER_MEDIUM_ATLAS]) || !resources.Get(volDC.srvInputs[srv::WATER_INTERFACE])) {
        return;
    }

    PostProcCB waterData{};
    FillUnderwaterConstants(waterData, ctx.environment.underwater);
    if (sharedPlanned) {
        waterData.underwaterOpticalMode = 1.0f;
        waterData.underwaterLightStrength = 1.0f;
        waterData.underwaterMaxDistance = 0.0f;
        waterData.underwaterLightSteps = 16.0f;
        for (const auto& visibleBody : h.waterMediumBodies) {
            if (visibleBody.waterInputIndex >= ctx.renderScene->water.size()) {
                continue;
            }
            const auto& body = ctx.renderScene->water[visibleBody.waterInputIndex];
            if (!body.medium.opticalEnabled) {
                continue;
            }
            waterData.underwaterMaxDistance = (std::max)(waterData.underwaterMaxDistance, body.medium.maxDistance);
            waterData.underwaterLightSteps = (std::max)(waterData.underwaterLightSteps, static_cast<float>(body.medium.lightSteps));
        }
    }
    // 材質の光学距離・ステップは媒体側の上限。PostProcess の品質予算と強度を迂回しない。
    waterData.underwaterMaxDistance = (std::min)(waterData.underwaterMaxDistance, vol.maxDist);
    waterData.underwaterLightSteps = (std::min)(waterData.underwaterLightSteps,
        static_cast<float>(std::clamp(vol.steps, 16, 64)));
    waterData.underwaterLightStrength *= vol.intensity;
    waterData.time = ctx.time;
    // Composite consumes the water result after applying Beer transmittance to the background.
    ctx.underwaterLightPassActive = dispatchLight(waterData);
}


void VolumetricLightPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    builder.Read("ShadowMap").Write("VolumetricRaw").Write("VolumetricResult");
    if (ctx.resourceRegistry.Target("PunctualShadowMap").IsValid()) {
        builder.Read("PunctualShadowMap");
    }
    if (ctx.resourceRegistry.Target("LightCookieAtlas").IsValid()) {
        builder.Read("LightCookieAtlas");
    }
    if (ctx.resourceRegistry.Target("WaterMediumAtlas").IsValid()) {
        builder.Read("WaterMediumAtlas");
    }
    if (ctx.resourceRegistry.Target("WaterInterface").IsValid()) {
        builder.Read("WaterInterface");
    }
    builder.ReadWrite("HDR");
}

bool VolumetricLightPass::IsEnabled(const RenderPassContext& ctx) const
{
    if (ctx.rayPathViewActive || !ctx.settings.IsVolumetricLightActive()) {
        return false;
    }
    const auto& water = ctx.environment.underwater;
    if (!UsesOpticalUnderwater(water)) {
        return true;
    }
    if (ctx.renderScene && HasOpticalWaterMedium(*ctx.renderScene)) {
        return HasOpticalWaterMedium(*ctx.renderScene, true);
    }
    return water.lightStrength > 0.0f;
}

void VolumetricLightPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteVolumetricLightPass(ctx);
}
} // namespace fbzz::renderer
