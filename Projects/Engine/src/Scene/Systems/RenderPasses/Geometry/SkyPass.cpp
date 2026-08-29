/// @file    RenderPasses/SkyPass.cpp
/// @brief   スカイドーム描画。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "GeometryPasses.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/SkyRenderer.hpp"
#include "Engine/Scene/Components/SunMoonRenderer.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Core/Time.hpp"

namespace fbzz::scene {

void ExecuteSkyPass(RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.skyShader.IsValid() || !h.skyVB.IsValid() || !h.skyIB.IsValid()) return;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* sky = go.GetComponent<SkyRenderer>();
        if (!sky || !sky->enabled) continue;

        PostProcCB skyPostData{};
        skyPostData.exposure = 1.0f;
        skyPostData.time     = Time::time;
        resources.Update(h.postprocCB, &skyPostData, sizeof(PostProcCB));

        AtmosphereCB atmData{};
        atmData.rayleighScattering[0] = sky->rayleighScattering.x;
        atmData.rayleighScattering[1] = sky->rayleighScattering.y;
        atmData.rayleighScattering[2] = sky->rayleighScattering.z;
        atmData.mieScattering         = sky->mieScattering;
        atmData.planetRadius          = sky->planetRadius;
        atmData.atmosphereRadius      = sky->atmosphereRadius;
        atmData.sunIntensity          = sky->skyScatterIntensity;
        atmData.mieG                  = sky->mieG;
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
        break;
    }
}

void ExecuteSunMoonPass(RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.sunMoonShader.IsValid() || !h.skyVB.IsValid() || !h.skyIB.IsValid()) return;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* sunMoon = go.GetComponent<SunMoonRenderer>();
        if (!sunMoon || !sunMoon->enabled) continue;

        AtmosphereCB atmData{};
        atmData.sunIntensity          = sunMoon->sunEnabled ? sunMoon->sunDiskIntensity : 0.0f;
        atmData.moonEnabled           = sunMoon->moonEnabled ? 1.0f : 0.0f;
        atmData.moonSize              = sunMoon->moonSize;
        atmData.moonBrightness        = sunMoon->moonBrightness;
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
        break;
    }
}

} // namespace fbzz::scene
