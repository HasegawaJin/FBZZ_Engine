/// @file    SSAOPass.cpp
/// @brief   Deferred GBuffer を入力に Screen Space Ambient Occlusion を生成する。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/ComputeCall.hpp>

namespace fbzz::scene {

void ExecuteSSAOPass(RenderPassContext& ctx)
{
    auto& r = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    const auto& ao = ctx.settings.postProcess.ambientOcclusion;

    if (!ao.enabled ||
        !h.ssaoShader.IsValid() || !h.ssaoBlurShader.IsValid() ||
        !h.ssaoRaw.IsValid() || !ctx.Res().Texture("SSAO").IsValid()) {
        return;
    }

    // WHAT: SSAO.cs.hlsl は GBuffer 法線と深度から遮蔽率を生成し、Blur で 4x4 平滑化する。
    // WHY: DeferredLighting は 1 枚の AO テクスチャだけを読む形にすると、Lighting pass の責務を
    //      PBR 合成に集中でき、Compute の UAV/SRV 競合もこのパス内に閉じられる。
    PostProcCB postData{};
    postData.texelSize[0] = 1.0f / static_cast<float>(ctx.width);
    postData.texelSize[1] = 1.0f / static_cast<float>(ctx.height);
    postData.screenSize[0] = static_cast<float>(ctx.width);
    postData.screenSize[1] = static_cast<float>(ctx.height);
    postData.ssaoIntensity = ao.intensity;
    resources.Update(h.postprocCB, &postData, sizeof(PostProcCB));

    renderer::ComputeCall ssaoDC;
    ssaoDC.shader = h.ssaoShader;
    ssaoDC.constantBuffers[0] = h.frameCB;
    ssaoDC.constantBuffers[5] = h.postprocCB;
    ssaoDC.srvInputs[6] = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 1);
    ssaoDC.srvInputs[7] = resources.GetDepthTexture(ctx.Res().Target("GBuffer"));
    ssaoDC.uavOutputs[0] = h.ssaoRaw;
    ssaoDC.dispatchX = (ctx.width / 2 + 7) / 8;
    ssaoDC.dispatchY = (ctx.height / 2 + 7) / 8;
    ssaoDC.dispatchZ = 1;
    r.Dispatch(ssaoDC, resources);

    renderer::ComputeCall blurDC;
    blurDC.shader = h.ssaoBlurShader;
    blurDC.constantBuffers[5] = h.postprocCB;
    blurDC.srvInputs[9] = h.ssaoRaw;
    blurDC.uavOutputs[0] = ctx.Res().Texture("SSAO");
    blurDC.dispatchX = (ctx.width / 2 + 7) / 8;
    blurDC.dispatchY = (ctx.height / 2 + 7) / 8;
    blurDC.dispatchZ = 1;
    r.Dispatch(blurDC, resources);
}

} // namespace fbzz::scene
