/// @file    RayLightExtractionTests.cpp
/// @brief   RT ライト入力の完全性、所有者、未対応状態と Raster との分離を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Systems/RenderLightExtractor.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <array>
#include <Graphics/Pipeline/RenderConstants.hpp>

namespace fbzz::tests {
namespace {
class RayLightExtractionTest : public testkit::EngineFixture {};

TEST_F(RayLightExtractionTest, DisabledExperimentalSessionKeepsRasterLightsWithoutRayListsOrDiagnostics)
{
    scene::Scene source;
    auto& point = source.CreateGameObject("Point").AddComponent<scene::LightComponent>();
    point.type = scene::LightComponent::Type::Point;
    point.castShadows = false;
    auto& sun = source.CreateGameObject("Sun").AddComponent<scene::LightComponent>();
    sun.castShadows = false;
    auto& sky = source.CreateGameObject("Sky").AddComponent<scene::SkyRenderer>();
    sky.dayNightEnabled = true;
    auto& emitter = source.CreateGameObject("ParticleLight").AddComponent<scene::ParticleEmitter>();
    emitter.settings.enabled = true;
    emitter.settings.light.lightEnabled = true;
    renderer::Camera camera;
    renderer::RenderSettings settings;
    settings.modeRequest.mode = renderer::RenderMode::PATH_TRACING;
    const auto disabled = scene::ExtractRenderLights(source, camera, settings, 1024);
    EXPECT_TRUE(disabled.rayLights.empty());
    EXPECT_FALSE(disabled.rayLightsComplete);
    ASSERT_EQ(disabled.punctualLights.size(), 1u);
    const auto enabled = scene::ExtractRenderLights(source, camera, settings, 1024, true);
    ASSERT_EQ(enabled.rayLights.size(), 3u);
    EXPECT_TRUE(enabled.rayLightsComplete);
    EXPECT_EQ(enabled.punctualLights.size(), disabled.punctualLights.size());
    EXPECT_VEC3_NEAR(enabled.punctualLights[0].position, disabled.punctualLights[0].position, testkit::kTolerance);
    EXPECT_FLOAT_EQ(enabled.punctualLights[0].intensity, disabled.punctualLights[0].intensity);
    EXPECT_VEC3_NEAR(enabled.lightData.lightColor, disabled.lightData.lightColor, testkit::kTolerance);
    EXPECT_FLOAT_EQ(enabled.lightData.lightIntensity, disabled.lightData.lightIntensity);
    EXPECT_TRUE(scene::ExtractRenderLights(source, camera, settings, 1024, false).rayLights.empty());
    EXPECT_EQ(settings.modeRequest.mode, renderer::RenderMode::PATH_TRACING);
}

TEST_F(RayLightExtractionTest, RayInputIsSceneCompleteBeyondRasterLimitAndRetainsEveryDirectionalOwner)
{
    scene::Scene source;
    constexpr uint32_t pointCount = renderer::kMaxPunctualLights + 4;
    for (uint32_t i = 0; i < pointCount; ++i) {
        auto& go = source.CreateGameObject("Point");
        auto& light = go.AddComponent<scene::LightComponent>();
        light.type = scene::LightComponent::Type::Point;
        light.castShadows = false;
        go.layer = 2;
        go.transform.worldPosition = {10000, 20000, 30000};
    }
    auto& first = source.CreateGameObject("SunA");
    first.AddComponent<scene::LightComponent>().castShadows = false;
    const auto owner = first.GetID();
    auto& second = source.CreateGameObject("SunB");
    second.AddComponent<scene::LightComponent>().castShadows = false;
    renderer::Camera camera;
    renderer::RenderSettings settings;
    const auto input = scene::ExtractRenderLights(source, camera, settings, 1024, true);
    EXPECT_EQ(input.punctualLights.size(), renderer::kMaxPunctualLights);
    ASSERT_EQ(input.rayLights.size(), pointCount + 2u);
    EXPECT_TRUE(input.rayLightsComplete);
    EXPECT_EQ(input.rayLights[0].layerMask, 4u);
    EXPECT_VEC3_NEAR(input.rayLights[0].position, (math::Vector3{10000, 20000, 30000}), testkit::kTolerance);
    EXPECT_EQ(input.rayLights[pointCount].type, renderer::RayLightType::DIRECTIONAL);
    EXPECT_EQ(input.rayLights[pointCount + 1].type, renderer::RayLightType::DIRECTIONAL);
    EXPECT_EQ(input.rayLights[pointCount].objectId.sceneGeneration, source.GetRenderSceneGeneration());
    EXPECT_EQ(input.rayLights[pointCount].objectId.index, owner.index);
    EXPECT_EQ(input.rayLights[pointCount].objectId.generation, owner.generation);
}

TEST_F(RayLightExtractionTest, RayInputPreservesRawAreaValuesAndDeclaredCookieBeforeGpuReadiness)
{
    scene::Scene source;
    auto& go = source.CreateGameObject("Area");
    auto& light = go.AddComponent<scene::LightComponent>();
    light.type = scene::LightComponent::Type::Area;
    light.castShadows = false;
    light.areaWidth = -2; light.areaHeight = 0; light.range = -1; light.intensity = -3;
    light.cookiePath = "unready-cookie.png";
    go.layer = -1;
    renderer::Camera camera;
    renderer::RenderSettings settings;
    const auto input = scene::ExtractRenderLights(source, camera, settings, 1024, true);
    ASSERT_EQ(input.rayLights.size(), 1u);
    const auto& ray = input.rayLights[0];
    EXPECT_EQ(ray.type, renderer::RayLightType::AREA);
    EXPECT_FLOAT_EQ(ray.areaWidth, -2);
    EXPECT_FLOAT_EQ(ray.areaHeight, 0);
    EXPECT_FLOAT_EQ(ray.range, -1);
    EXPECT_FLOAT_EQ(ray.intensity, -3);
    EXPECT_EQ(ray.layerMask, 0u);
    EXPECT_NE(ray.unsupportedFlags & renderer::RAY_LIGHT_UNSUPPORTED_COOKIE, 0u);
    EXPECT_NE(ray.unsupportedFlags & renderer::RAY_LIGHT_INVALID_LAYER, 0u);
    EXPECT_EQ(input.lightCookieViewCount, 0);
}

TEST_F(RayLightExtractionTest, RayInputReportsParticleSourceWithoutRequiringLiveCpuParticles)
{
    scene::Scene source;
    auto& go = source.CreateGameObject("ParticleLight");
    auto& emitter = go.AddComponent<scene::ParticleEmitter>();
    emitter.settings.enabled = true;
    emitter.settings.light.lightEnabled = true;
    go.layer = 3;
    renderer::Camera camera;
    renderer::RenderSettings settings;
    const auto input = scene::ExtractRenderLights(source, camera, settings, 1024, true);
    ASSERT_EQ(input.rayLights.size(), 1u);
    EXPECT_EQ(input.rayLights[0].layerMask, 8u);
    EXPECT_NE(input.rayLights[0].unsupportedFlags & renderer::RAY_LIGHT_UNSUPPORTED_PARTICLE, 0u);
    EXPECT_TRUE(input.punctualLights.empty());
    go.SetActive(false);
    EXPECT_TRUE(scene::ExtractRenderLights(source, camera, settings, 1024, true).rayLights.empty());
}

TEST_F(RayLightExtractionTest, RayInputKeepsEveryOwnerShadowSettingIndependentOfSelectedDirectional)
{
    scene::Scene source;
    const std::array<float, 6> strengths{0.1f, 0.25f, 0.5f, 0.75f, 1.5f, -0.5f};
    for (uint32_t type = 0; type < strengths.size(); ++type) {
        auto& go = source.CreateGameObject("Light");
        auto& light = go.AddComponent<scene::LightComponent>();
        light.type = static_cast<scene::LightComponent::Type>(type);
        light.castShadows = type != 0;
        light.shadowStrength = strengths[type];
    }
    renderer::Camera camera;
    renderer::RenderSettings settings;
    settings.shadowEnabled = true;
    auto input = scene::ExtractRenderLights(source, camera, settings, 1024, true);
    ASSERT_EQ(input.rayLights.size(), strengths.size());
    for (size_t i = 0; i < strengths.size(); ++i) {
        EXPECT_EQ(input.rayLights[i].castShadows, i != 0);
        EXPECT_FLOAT_EQ(input.rayLights[i].shadowStrength, strengths[i]);
    }
    EXPECT_FALSE(input.dirCastShadows);
    settings.shadowEnabled = false;
    input = scene::ExtractRenderLights(source, camera, settings, 1024, true);
    ASSERT_EQ(input.rayLights.size(), strengths.size());
    for (size_t i = 0; i < strengths.size(); ++i) {
        EXPECT_FALSE(input.rayLights[i].castShadows);
        EXPECT_FLOAT_EQ(input.rayLights[i].shadowStrength, strengths[i]);
    }
}

TEST_F(RayLightExtractionTest, BlackSkyDayNightCurveIsExplicitlyUnsupportedWithOrWithoutDirectionalOwner)
{
    scene::Scene source;
    auto& skyOwner = source.CreateGameObject("BlackSky");
    auto& sky = skyOwner.AddComponent<scene::SkyRenderer>();
    sky.skyScatterIntensity = 0;
    sky.dayNightEnabled = true;
    const auto skyId = skyOwner.GetID();
    renderer::Camera camera;
    renderer::RenderSettings settings;
    auto input = scene::ExtractRenderLights(source, camera, settings, 1024, true);
    ASSERT_EQ(input.rayLights.size(), 1u);
    EXPECT_NE(input.rayLights[0].unsupportedFlags & renderer::RAY_LIGHT_UNSUPPORTED_DAY_NIGHT, 0u);
    EXPECT_EQ(input.rayLights[0].objectId.index, skyId.index);
    auto& sunOwner = source.CreateGameObject("Directional");
    auto& sun = sunOwner.AddComponent<scene::LightComponent>();
    sun.castShadows = false;
    const auto sunId = sunOwner.GetID();
    input = scene::ExtractRenderLights(source, camera, settings, 1024, true);
    ASSERT_EQ(input.rayLights.size(), 1u);
    EXPECT_NE(input.rayLights[0].unsupportedFlags & renderer::RAY_LIGHT_UNSUPPORTED_DAY_NIGHT, 0u);
    EXPECT_EQ(input.rayLights[0].objectId.index, sunId.index);
    EXPECT_FLOAT_EQ(input.rayLights[0].intensity, sun.intensity);
    sky.dayNightEnabled = false;
    input = scene::ExtractRenderLights(source, camera, settings, 1024, true);
    ASSERT_EQ(input.rayLights.size(), 1u);
    EXPECT_EQ(input.rayLights[0].unsupportedFlags, 0u);
}

}
}
