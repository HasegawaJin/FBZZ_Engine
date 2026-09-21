/// @file    RenderLightExtractorTests.cpp
/// @brief   ライト抽出のワールド値、無効階層、影と Cookie の対応を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Systems/RenderLightExtractor.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Renderer/ColorTemperature.hpp>

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
}
}
