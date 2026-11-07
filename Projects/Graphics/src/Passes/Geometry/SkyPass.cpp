/// @file    SkyPass.cpp
/// @brief   スカイドーム描画。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include "Graphics/Renderer/DrawCall.hpp"

namespace fbzz::renderer {
namespace {
AtmosphereCB SkyAtmosphere(const RenderSkyInput* sky)
{
    AtmosphereCB data{};
    data.planetRadius=sky?sky->planetRadius:6371.0f;
    data.atmosphereRadius=sky?sky->atmosphereRadius:6471.0f;
    if(sky){
        data.rayleighScattering[0]=sky->rayleighScattering.x;
        data.rayleighScattering[1]=sky->rayleighScattering.y;
        data.rayleighScattering[2]=sky->rayleighScattering.z;
        data.mieScattering=sky->mieScattering;data.sunIntensity=sky->skyScatterIntensity;data.mieG=sky->mieG;
    }
    return data;
}
} /// @note namespace

void ExecuteSkyPass(RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.skyShader.IsValid() || !h.skyVB.IsValid() || !h.skyIB.IsValid()) return;

    if (ctx.environment.sky) {
        const auto* sky = &*ctx.environment.sky;

        PostProcCB skyPostData{};
        skyPostData.exposure = 1.0f;
        skyPostData.time     = ctx.time;
        resources.Update(h.postprocCB, &skyPostData, sizeof(PostProcCB));

        const auto atmData = SkyAtmosphere(sky);
        resources.Update(h.atmosphereCB, &atmData, sizeof(AtmosphereCB));

        renderer::DrawCall dc;
        dc.vertexBuffer       = h.skyVB;
        dc.indexBuffer        = h.skyIB;
        dc.indexCount         = h.skyIndexCount;
        dc.shader             = h.skyShader;
        dc.pipelineState      = h.skyPSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[3] = h.lightCB;
        dc.constantBuffers[5] = h.postprocCB;
        dc.constantBuffers[6] = h.atmosphereCB;
        SubmitCounted(ctx, dc);
    }
}

void ExecuteSunMoonPass(RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.sunMoonShader.IsValid() || !h.skyVB.IsValid() || !h.skyIB.IsValid()) return;

    if (ctx.environment.sunMoon) {
        const auto* sunMoon = &*ctx.environment.sunMoon;

        const auto skyData = SkyAtmosphere(ctx.environment.sky ? &*ctx.environment.sky : nullptr);
        auto atmData = skyData;
        atmData.sunIntensity          = sunMoon->sunEnabled ? sunMoon->sunDiskIntensity : 0.0f;
        atmData.moonEnabled           = sunMoon->moonEnabled ? 1.0f : 0.0f;
        atmData.moonSize              = sunMoon->moonSize;
        atmData.moonBrightness        = sunMoon->moonBrightness;
        atmData.starBrightness        = sunMoon->starBrightness;
        atmData.moonColor[0]          = sunMoon->moonColor.x;
        atmData.moonColor[1]          = sunMoon->moonColor.y;
        atmData.moonColor[2]          = sunMoon->moonColor.z;
        resources.Update(h.atmosphereCB, &atmData, sizeof(AtmosphereCB));

        renderer::DrawCall dc;
        dc.vertexBuffer       = h.skyVB;
        dc.indexBuffer        = h.skyIB;
        dc.indexCount         = h.skyIndexCount;
        dc.shader             = h.sunMoonShader;
        dc.pipelineState      = h.sunMoonPSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[3] = h.lightCB;
        dc.constantBuffers[6] = h.atmosphereCB;
        SubmitCounted(ctx, dc);
        /// @note 後段の大気フォグが同じb6を読むため、ディスクの輝度と空の散乱係数を混同させない。
        resources.Update(h.atmosphereCB, &skyData, sizeof(skyData));
    }
}


std::string_view SkyPass::Name() const { return "Sky"; }

void SkyPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    /// @note 深度が最遠 (Reversed-Z で 0) の画素だけを埋めるので、既に描かれた不透明を読み書きする。
    builder.ReadWrite("HDR");
}

void SkyPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteSkyPass(ctx);
}

std::string_view SunMoonPass::Name() const { return "SunMoon"; }

void SunMoonPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.ReadWrite("HDR");
}

void SunMoonPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteSunMoonPass(ctx);
}

} /// @note namespace fbzz::renderer
