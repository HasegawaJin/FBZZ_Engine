/// @file    RenderEnvironmentExtractor.cpp
/// @brief   視点ごとの空・環境光と水中・雲の描画入力を組み立てる。
/// @author  Hasegawa Jin
/// @date    2026-09-21

#include "RenderCandidateLease.hpp"
#include "RenderEnvironmentExtraction.hpp"
#include <Engine/Scene/Systems/RenderEnvironmentExtractor.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Scene/Components/SunMoonRenderer.hpp>
#include <Engine/Scene/Components/EnvironmentLightComponent.hpp>
#include <Engine/Scene/Components/ParticleColorSpace.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include "RenderExtractionPhases.hpp"
#include <algorithm>

namespace fbzz::scene {

namespace {

void ExtractRayEnvironment(RenderPassContext& context, renderer::RenderEnvironmentInput& output)
{
    static renderer::RayEnvironmentCache rayEnvironmentCache;
    if (!context.experimentalRayTracingEnabled) rayEnvironmentCache.Reset();
    /// @note Global IBL と同じ component order/active 条件で同じ owner を選ぶ。
    if (context.experimentalRayTracingEnabled) for (const EntityID entity : context.scene.GetEntities<EnvironmentLightComponent>()) {
        const auto* ownerObject = context.scene.GetGameObject(entity);
        const auto* light = context.scene.GetComponent<EnvironmentLightComponent>(entity);
        if (!ownerObject || !ownerObject->activeInHierarchy() || !light || !light->enabled) continue;
        auto& environment = output.rayEnvironment;
        environment.requested = light->source != IblSource::StaticDDS || !light->rawEnvironmentPath.empty();
        environment.intensity = light->intensity;
        environment.rotationRadians = light->rotationY * 0.017453292519943295f;
        if (light->source == IblSource::StaticDDS && !light->rawEnvironmentPath.empty()) {
            auto& resolver = asset::StreamedTextureResolver::Engine();
            if (const auto* texture = resolver.ResolveAsset(context.resources, light->rawEnvironmentPath)) {
                environment.rawTexture = texture->gpuHandle;
                environment.pixels = rayEnvironmentCache.Load(texture->sourcePath, texture->gpuHandle,
                    resolver.ResolveContentRevision(context.resources, light->rawEnvironmentPath), context.resources);
                environment.ready = environment.pixels != nullptr;
            }
        }
        break;
    }
}

void ExtractSkyAndSunMoon(const RenderPassContext& context, renderer::RenderEnvironmentInput& output)
{
    if (!context.scene.GetEntities<SkyRenderer>().empty()
        || !context.scene.GetEntities<SunMoonRenderer>().empty()) {
        detail::RenderCandidateLease candidateLease;
        auto& candidates = candidateLease.Entities();
        const auto skies = context.scene.GetEntities<SkyRenderer>();
        const auto suns = context.scene.GetEntities<SunMoonRenderer>();
        candidates.assign(skies.begin(), skies.end());
        candidates.insert(candidates.end(), suns.begin(), suns.end());
        /// @note 後から追加されたコンポーネントにも GameObject の走査順を適用し、無関係な骨ノードの走査を省く。
        context.scene.SortEntitiesBySceneOrder(candidates);
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        for (const EntityID entity : candidates) {
            const auto* ownerObject = context.scene.GetGameObject(entity);
            if (!ownerObject || !ShouldRenderGameObject(*ownerObject, context.cullingMask)) continue;
            if (!output.sky) if (const auto* sky = context.scene.GetComponent<SkyRenderer>(entity); sky && sky->enabled)
                output.sky = renderer::RenderSkyInput{sky->rayleighScattering, sky->mieScattering,
                    sky->planetRadius, sky->atmosphereRadius, sky->skyScatterIntensity, sky->mieG};
            if (!output.sunMoon) if (const auto* sun = context.scene.GetComponent<SunMoonRenderer>(entity); sun && sun->enabled)
                output.sunMoon = renderer::RenderSunMoonInput{sun->sunEnabled, sun->sunDiskIntensity,
                    sun->moonEnabled, sun->moonSize, sun->moonBrightness, sun->moonColor, sun->starBrightness};
        }
    }
}

void ResolveLightCookieTextures(RenderPassContext& context)
{
    for (int cookieIndex = 0; cookieIndex < context.lightCookieViewCount; ++cookieIndex) {
        auto& cookie = context.lightCookieViews[cookieIndex];
        cookie.sourceTexture = asset::StreamedTextureResolver::Engine().ResolveGpu(context.resources, cookie.sourcePath);
        cookie.srgb = IsEffectTextureSrgb(cookie.sourcePath);
    }
}

}

renderer::RenderEnvironmentInput ExtractRenderEnvironment(RenderPassContext& context)
{
    using namespace environment_extraction_detail;

    detail::RenderExtractionPhases phases("RenderSceneExtractor::Environment");
    phases.Begin("RenderSceneExtractor::Environment::WaterPrepare");
    PrepareWaterSceneSurfaces(context.scene);
    phases.End();
    renderer::RenderEnvironmentInput output;
    phases.Begin("RenderSceneExtractor::Environment::RayEnvironment");
    ExtractRayEnvironment(context, output);
    phases.Begin("RenderSceneExtractor::Environment::SkySunCollect");
    ExtractSkyAndSunMoon(context, output);
    phases.Begin("RenderSceneExtractor::Environment::UnderwaterAndCaustics");
    output.underwater = EvaluateUnderwaterInfo(context);
    output.cameraUnderwater = output.underwater.enabled;
    output.caustics = FindCausticsSource(context);
    phases.Begin("RenderSceneExtractor::Environment::CloudAndLightCookies");
    ExtractCloud(context, output);
    ResolveLightCookieTextures(context);
    phases.End();
    return output;
}

}
