// FBZZ Engine
// RenderPasses/SkyPass.cpp | fbzz::scene
// スカイドーム描画
#include "GeometryPasses.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/SkyRenderer.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Core/Time.hpp"

namespace fbzz::scene {

void ExecuteSkyPass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.skyShader.IsValid() || !h.skyVB.IsValid() || !h.skyIB.IsValid()) return;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* sky = go.GetComponent<SkyRenderer>();
        if (!sky || !sky->enabled) continue;

        PostProcCB skyPostData{};
        skyPostData.exposure = 1.0f;
        skyPostData.time     = core::Time::TotalTime();
        resources.Update(h.postprocCB, &skyPostData, sizeof(PostProcCB));

        AtmosphereCB atmData{};
        atmData.rayleighScattering[0] = sky->rayleighScattering.x;
        atmData.rayleighScattering[1] = sky->rayleighScattering.y;
        atmData.rayleighScattering[2] = sky->rayleighScattering.z;
        atmData.mieScattering         = sky->mieScattering;
        atmData.planetRadius          = 6371.0f;
        atmData.atmosphereRadius      = 6471.0f;
        atmData.sunIntensity          = sky->sunIntensity;
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
        renderer.Submit(dc, resources);
        break;
    }
}

} // namespace fbzz::scene
