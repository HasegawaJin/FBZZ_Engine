/// @file    RenderLightExtractorTests.cpp
/// @brief   ライト抽出のワールド値、無効階層、影と Cookie の対応を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Systems/RenderLightExtractor.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/SkyRenderer.hpp>
#include <Engine/Renderer/ColorTemperature.hpp>
#include <array>

namespace fbzz::tests {
namespace {
class RenderLightExtractorTest : public testkit::EngineFixture {};

TEST_F(RenderLightExtractorTest, CopiesWorldPositionAndTemperatureWithoutKeepingComponentReferences)
{
    scene::Scene source;
    auto& go = source.CreateGameObject("Point");
    auto& light = go.AddComponent<scene::LightComponent>();
    light.type = scene::LightComponent::Type::Point;
    light.useColorTemperature = true;
    light.colorTemperature = 2700;
    light.castShadows = false;
    go.transform.worldPosition = {10, 2, -3};
    renderer::Camera camera;
    renderer::RenderSettings settings;
    const auto input = scene::ExtractRenderLights(source, camera, settings, 1024);
    ASSERT_EQ(input.punctualLights.size(), 1u);
    light.intensity = 99;
    go.transform.worldPosition = {};
    EXPECT_FLOAT_EQ(input.punctualLights[0].intensity, 3);
    EXPECT_VEC3_NEAR(input.punctualLights[0].position, (math::Vector3{10, 2, -3}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(input.punctualLights[0].color, renderer::ColorFromTemperature(2700), testkit::kTolerance);
    EXPECT_EQ(input.punctualLights[0].shadowIndex, -1);
    EXPECT_EQ(input.legacyShadowSlots[0], -1);
}

TEST_F(RenderLightExtractorTest, DisabledLightAndInactiveParentDoNotContribute)
{
    scene::Scene source;
    auto& parent = source.CreateGameObject("Parent");
    auto& go = source.CreateGameObject("Point");
    go.SetParent(parent);
    auto& light = go.AddComponent<scene::LightComponent>();
    light.type = scene::LightComponent::Type::Point;
    renderer::Camera camera;
    renderer::RenderSettings settings;
    parent.SetActive(false);
    EXPECT_TRUE(scene::ExtractRenderLights(source, camera, settings, 1024).punctualLights.empty());
    parent.SetActive(true);
    light.enabled = false;
    EXPECT_TRUE(scene::ExtractRenderLights(source, camera, settings, 1024).punctualLights.empty());
}

TEST_F(RenderLightExtractorTest, SpotShadowAndCookieSlotsMatchLegacyAndStructuredInputs)
{
    scene::Scene source;
    auto& go = source.CreateGameObject("Spot");
    auto& light = go.AddComponent<scene::LightComponent>();
    light.type = scene::LightComponent::Type::Spot;
    light.cookiePath = "cookie.png";
    renderer::Camera camera;
    renderer::RenderSettings settings;
    const auto input = scene::ExtractRenderLights(source, camera, settings, 1024);
    ASSERT_EQ(input.punctualLights.size(), 1u);
    EXPECT_EQ(input.punctualShadowViewCount, 1);
    EXPECT_EQ(input.lightCookieViewCount, 1);
    EXPECT_EQ(input.legacyShadowSlots[8], input.punctualLights[0].shadowIndex);
    EXPECT_EQ(input.legacyCookieSlots[8], input.punctualLights[0].cookieIndex);
    EXPECT_EQ(input.lightCookieViews[0].sourcePath, "cookie.png");
}

TEST_F(RenderLightExtractorTest, RayInputIsSceneCompleteBeyondRasterLimitAndRetainsEveryDirectionalOwner)
{
    scene::Scene source;
    for (uint32_t i = 0; i < 260; ++i) {
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
    const auto input = scene::ExtractRenderLights(source, camera, settings, 1024);
    EXPECT_EQ(input.punctualLights.size(), 256u);
    ASSERT_EQ(input.rayLights.size(), 262u);
    EXPECT_TRUE(input.rayLightsComplete);
    EXPECT_EQ(input.rayLights[0].layerMask, 4u);
    EXPECT_VEC3_NEAR(input.rayLights[0].position, (math::Vector3{10000, 20000, 30000}), testkit::kTolerance);
    EXPECT_EQ(input.rayLights[260].type, renderer::RayLightType::DIRECTIONAL);
    EXPECT_EQ(input.rayLights[261].type, renderer::RayLightType::DIRECTIONAL);
    EXPECT_EQ(input.rayLights[260].objectId.sceneGeneration, source.GetRenderSceneGeneration());
    EXPECT_EQ(input.rayLights[260].objectId.index, owner.index);
    EXPECT_EQ(input.rayLights[260].objectId.generation, owner.generation);
}

TEST_F(RenderLightExtractorTest, RayInputPreservesRawAreaValuesAndDeclaredCookieBeforeGpuReadiness)
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
    const auto input = scene::ExtractRenderLights(source, camera, settings, 1024);
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

TEST_F(RenderLightExtractorTest, RayInputReportsParticleSourceWithoutRequiringLiveCpuParticles)
{
    scene::Scene source;
    auto& go = source.CreateGameObject("ParticleLight");
    auto& emitter = go.AddComponent<scene::ParticleEmitter>();
    emitter.settings.enabled = true;
    emitter.settings.light.lightEnabled = true;
    go.layer = 3;
    renderer::Camera camera;
    renderer::RenderSettings settings;
    const auto input = scene::ExtractRenderLights(source, camera, settings, 1024);
    ASSERT_EQ(input.rayLights.size(), 1u);
    EXPECT_EQ(input.rayLights[0].layerMask, 8u);
    EXPECT_NE(input.rayLights[0].unsupportedFlags & renderer::RAY_LIGHT_UNSUPPORTED_PARTICLE, 0u);
    EXPECT_TRUE(input.punctualLights.empty());
    go.SetActive(false);
    EXPECT_TRUE(scene::ExtractRenderLights(source, camera, settings, 1024).rayLights.empty());
}

TEST_F(RenderLightExtractorTest, RayInputKeepsEveryOwnerShadowSettingIndependentOfSelectedDirectional)
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
    auto input = scene::ExtractRenderLights(source, camera, settings, 1024);
    ASSERT_EQ(input.rayLights.size(), strengths.size());
    for (size_t i = 0; i < strengths.size(); ++i) {
        EXPECT_EQ(input.rayLights[i].castShadows, i != 0);
        EXPECT_FLOAT_EQ(input.rayLights[i].shadowStrength, strengths[i]);
    }
    EXPECT_FALSE(input.dirCastShadows);
    settings.shadowEnabled = false;
    input = scene::ExtractRenderLights(source, camera, settings, 1024);
    ASSERT_EQ(input.rayLights.size(), strengths.size());
    for (size_t i = 0; i < strengths.size(); ++i) {
        EXPECT_FALSE(input.rayLights[i].castShadows);
        EXPECT_FLOAT_EQ(input.rayLights[i].shadowStrength, strengths[i]);
    }
}

TEST_F(RenderLightExtractorTest, BlackSkyDayNightCurveIsExplicitlyUnsupportedWithOrWithoutDirectionalOwner)
{
    scene::Scene source;
    auto& skyOwner = source.CreateGameObject("BlackSky");
    auto& sky = skyOwner.AddComponent<scene::SkyRenderer>();
    sky.skyScatterIntensity = 0;
    sky.dayNightEnabled = true;
    const auto skyId = skyOwner.GetID();
    renderer::Camera camera;
    renderer::RenderSettings settings;
    auto input = scene::ExtractRenderLights(source, camera, settings, 1024);
    ASSERT_EQ(input.rayLights.size(), 1u);
    EXPECT_NE(input.rayLights[0].unsupportedFlags & renderer::RAY_LIGHT_UNSUPPORTED_DAY_NIGHT, 0u);
    EXPECT_EQ(input.rayLights[0].objectId.index, skyId.index);
    auto& sunOwner = source.CreateGameObject("Directional");
    auto& sun = sunOwner.AddComponent<scene::LightComponent>();
    sun.castShadows = false;
    const auto sunId = sunOwner.GetID();
    input = scene::ExtractRenderLights(source, camera, settings, 1024);
    ASSERT_EQ(input.rayLights.size(), 1u);
    EXPECT_NE(input.rayLights[0].unsupportedFlags & renderer::RAY_LIGHT_UNSUPPORTED_DAY_NIGHT, 0u);
    EXPECT_EQ(input.rayLights[0].objectId.index, sunId.index);
    EXPECT_FLOAT_EQ(input.rayLights[0].intensity, sun.intensity);
    sky.dayNightEnabled = false;
    input = scene::ExtractRenderLights(source, camera, settings, 1024);
    ASSERT_EQ(input.rayLights.size(), 1u);
    EXPECT_EQ(input.rayLights[0].unsupportedFlags, 0u);
}

} /// @note namespace
} /// @note namespace fbzz::tests
