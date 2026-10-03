/// @file    RayEnvironmentTests.cpp
/// @brief   Raw cube solid angles, actual CDF intervals and orientation PDF contracts.
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <TestKit/TestKit.hpp>
#include <Graphics/RayTracing/RayEnvironment.hpp>
#include <Graphics/Renderer/Camera.hpp>
#include <Graphics/Renderer/RenderEnvironment.hpp>
#include <cmath>
#include <limits>

namespace fbzz::tests {
class RayEnvironmentTest : public testkit::Fixture {
protected:
    renderer::RayEnvironmentInput MakeInput(uint32_t size, std::array<float, 3> value)
    {
        auto pixels = std::make_shared<renderer::RayEnvironmentPixels>();
        pixels->faceSize = size;
        pixels->contentVersion = 7;
        pixels->radiance.resize(static_cast<size_t>(size) * size * 6, value);
        renderer::RayEnvironmentInput input;
        input.requested = input.ready = true;
        input.pixels = std::move(pixels);
        return input;
    }
};

TEST_F(RayEnvironmentTest, CubeSolidAnglesCoverSphereAndSymmetricCells)
{
    constexpr double pi = 3.14159265358979323846;
    for (uint32_t size : {1u, 8u, 128u}) {
        double total = 0;
        for (uint32_t y = 0; y < size; ++y) for (uint32_t x = 0; x < size; ++x) {
            const double angle = renderer::RayCubeTexelSolidAngle(x, y, size);
            EXPECT_GT(angle, 0);
            EXPECT_NEAR(angle, renderer::RayCubeTexelSolidAngle(size - 1 - x, size - 1 - y, size), 1e-12);
            total += 6 * angle;
        }
        EXPECT_NEAR(total, 4 * pi, 1e-10);
    }
}
TEST_F(RayEnvironmentTest, BlackCubeRetainsFullSamplingSupportAtMaximumTableSize)
{
    renderer::RayEnvironmentDistribution result;
    ASSERT_TRUE(renderer::BuildRayEnvironmentDistribution(MakeInput(128, {0, 0, 0}), result));
    float previous = 0;
    for (const auto& record : result.records) {
        EXPECT_GT(record.selectionPdf, 0);
        EXPECT_FLOAT_EQ(record.selectionPdf, record.selectionCdf - previous);
        previous = record.selectionCdf;
    }
    EXPECT_FLOAT_EQ(previous, 1);
    EXPECT_EQ(result.contentVersion, 7u);
}
TEST_F(RayEnvironmentTest, BrightTexelImportanceUsesLuminanceAndRoundedInterval)
{
    auto input = MakeInput(1, {0, 0, 0});
    auto pixels = std::make_shared<renderer::RayEnvironmentPixels>(*input.pixels);
    pixels->radiance[0] = {100, 100, 100};
    input.pixels = pixels;
    renderer::RayEnvironmentDistribution result;
    ASSERT_TRUE(renderer::BuildRayEnvironmentDistribution(input, result));
    EXPECT_NEAR(result.records[0].selectionPdf, 0.95f + 0.05f / 6, 1e-6f);
    for (size_t i = 1; i < 6; ++i) EXPECT_NEAR(result.records[i].selectionPdf, 0.05f / 6, 1e-6f);
    EXPECT_NEAR(renderer::RayEnvironmentDirectionPdf(result, {1, 0, 0}), result.records[0].selectionPdf * 0.25f, 1e-6f);
    constexpr float halfPi = 1.57079632679489661923f;
    EXPECT_NEAR(renderer::RayEnvironmentDirectionPdf(result, {0, 0, -1}, halfPi),
        result.records[0].selectionPdf * 0.25f, 1e-6f);
}
TEST_F(RayEnvironmentTest, DirectionPdfIntegratesToOneWithCubeJacobian)
{
    renderer::RayEnvironmentDistribution result;
    ASSERT_TRUE(renderer::BuildRayEnvironmentDistribution(MakeInput(8, {1, 2, 3}), result));
    double integral = 0;
    constexpr uint32_t subdivisions = 64;
    for (uint32_t face = 0; face < 6; ++face) for (uint32_t y = 0; y < subdivisions; ++y)
        for (uint32_t x = 0; x < subdivisions; ++x) {
            const float u = 2 * (x + 0.5f) / subdivisions - 1;
            const float v = 2 * (y + 0.5f) / subdivisions - 1;
            integral += renderer::RayEnvironmentDirectionPdf(result, renderer::RayCubeDirection(face, u, v))
                * renderer::RayCubeTexelSolidAngle(x, y, subdivisions);
        }
    EXPECT_NEAR(integral, 1, 3e-4);
}
TEST_F(RayEnvironmentTest, InvalidInputLeavesPublishedDistributionUnchanged)
{
    auto input = MakeInput(1, {1, 1, 1});
    renderer::RayEnvironmentDistribution result;
    ASSERT_TRUE(renderer::BuildRayEnvironmentDistribution(input, result));
    const auto saved = result.records;
    input.intensity = -1;
    EXPECT_FALSE(renderer::BuildRayEnvironmentDistribution(input, result));
    EXPECT_EQ(result.records.size(), saved.size());
    EXPECT_FLOAT_EQ(result.records[0].selectionPdf, saved[0].selectionPdf);
    input.intensity = 1;
    auto invalid = std::make_shared<renderer::RayEnvironmentPixels>(*input.pixels);
    invalid->contentVersion = 0;
    input.pixels = invalid;
    EXPECT_FALSE(renderer::BuildRayEnvironmentDistribution(input, result));
    EXPECT_FLOAT_EQ(result.records[0].selectionPdf, saved[0].selectionPdf);
    invalid->contentVersion = 7;
    invalid->radiance[2][1] = std::numeric_limits<float>::infinity();
    input.pixels = invalid;
    EXPECT_FALSE(renderer::BuildRayEnvironmentDistribution(input, result));
    EXPECT_FLOAT_EQ(result.records[0].selectionPdf, saved[0].selectionPdf);
    input.ready = false;
    EXPECT_FALSE(renderer::BuildRayEnvironmentDistribution(input, result));
}

TEST_F(RayEnvironmentTest, ResolvesLinearHdrCameraRadianceIndependentlyOfBackgroundAlpha)
{
    renderer::RenderEnvironmentInput environment;
    renderer::Camera camera;
    camera.m_backgroundColor = {2, 4, 8, 0};
    math::Vector3 radiance{7, 8, 9};
    ASSERT_TRUE(renderer::ResolveRayConstantEnvironment(environment, camera, radiance));
    EXPECT_VEC3_NEAR(radiance, (math::Vector3{2, 4, 8}), 0.0f);
    camera.m_backgroundColor.w = std::numeric_limits<float>::quiet_NaN();
    ASSERT_TRUE(renderer::ResolveRayConstantEnvironment(environment, camera, radiance));
    EXPECT_VEC3_NEAR(radiance, (math::Vector3{2, 4, 8}), 0.0f);
}

TEST_F(RayEnvironmentTest, ExplicitBlackSkyOverridesCameraRgbButDepthOnlyWithoutSkyIsUnknown)
{
    renderer::RenderEnvironmentInput environment;
    renderer::Camera camera;
    camera.m_backgroundColor = {2, 4, 8, 1};
    camera.m_clearMode = renderer::CameraClearMode::DepthOnly;
    math::Vector3 radiance{7, 8, 9};
    EXPECT_FALSE(renderer::ResolveRayConstantEnvironment(environment, camera, radiance));
    EXPECT_VEC3_NEAR(radiance, (math::Vector3{7, 8, 9}), 0.0f);
    environment.sky = renderer::RenderSkyInput{};
    ASSERT_TRUE(renderer::ResolveRayConstantEnvironment(environment, camera, radiance));
    EXPECT_VEC3_NEAR(radiance, math::Vector3::ZERO, 0.0f);
}

TEST_F(RayEnvironmentTest, RawEnvironmentRequestsNeverBecomeConstantWhenPendingOrReady)
{
    renderer::RenderEnvironmentInput environment;
    environment.rayEnvironment.requested = true;
    renderer::Camera camera;
    math::Vector3 radiance{7, 8, 9};
    for (bool ready : {false, true}) {
        environment.rayEnvironment.ready = ready;
        EXPECT_FALSE(renderer::ResolveRayConstantEnvironment(environment, camera, radiance));
        EXPECT_VEC3_NEAR(radiance, (math::Vector3{7, 8, 9}), 0.0f);
    }
}

TEST_F(RayEnvironmentTest, EmittingSkyDisksAndCloudLeaveConstantEnvironmentUnknown)
{
    renderer::Camera camera;
    for (uint32_t source = 0; source < 4; ++source) {
        SCOPED_TRACE(source);
        renderer::RenderEnvironmentInput environment;
        if (source == 0) {
            environment.sky = renderer::RenderSkyInput{};
            environment.sky->skyScatterIntensity = 1;
        } else if (source == 3) environment.cloudEnabled = true;
        else {
            environment.sunMoon = renderer::RenderSunMoonInput{};
            if (source == 1) {
                environment.sunMoon->sunEnabled = true;
                environment.sunMoon->sunDiskIntensity = 1;
            } else {
                environment.sunMoon->moonEnabled = true;
                environment.sunMoon->moonBrightness = 1;
            }
        }
        math::Vector3 radiance{7, 8, 9};
        EXPECT_FALSE(renderer::ResolveRayConstantEnvironment(environment, camera, radiance));
        EXPECT_VEC3_NEAR(radiance, (math::Vector3{7, 8, 9}), 0.0f);
    }
    renderer::RenderEnvironmentInput environment;
    environment.sunMoon = renderer::RenderSunMoonInput{};
    environment.sunMoon->sunEnabled = true;
    environment.sunMoon->moonEnabled = true;
    math::Vector3 radiance;
    EXPECT_TRUE(renderer::ResolveRayConstantEnvironment(environment, camera, radiance));
    environment.sunMoon->sunEnabled = false;
    environment.sunMoon->moonEnabled = false;
    environment.sunMoon->sunDiskIntensity = 10;
    environment.sunMoon->moonBrightness = 10;
    EXPECT_TRUE(renderer::ResolveRayConstantEnvironment(environment, camera, radiance));
}

TEST_F(RayEnvironmentTest, InvalidCameraRgbOrActiveSkyIntensityDoesNotModifyOutput)
{
    for (float invalid : {-1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        renderer::RenderEnvironmentInput environment;
        renderer::Camera camera;
        for (uint32_t channel = 0; channel < 3; ++channel) {
            SCOPED_TRACE(channel);
            camera.m_backgroundColor = {1, 2, 3, 1};
            if (channel == 0) camera.m_backgroundColor.x = invalid;
            else if (channel == 1) camera.m_backgroundColor.y = invalid;
            else camera.m_backgroundColor.z = invalid;
            math::Vector3 radiance{7, 8, 9};
            EXPECT_FALSE(renderer::ResolveRayConstantEnvironment(environment, camera, radiance));
            EXPECT_VEC3_NEAR(radiance, (math::Vector3{7, 8, 9}), 0.0f);
        }
        camera.m_backgroundColor = {1, 2, 3, 1};
        environment.sky = renderer::RenderSkyInput{};
        environment.sky->skyScatterIntensity = invalid;
        math::Vector3 radiance{7, 8, 9};
        EXPECT_FALSE(renderer::ResolveRayConstantEnvironment(environment, camera, radiance));
        EXPECT_VEC3_NEAR(radiance, (math::Vector3{7, 8, 9}), 0.0f);
    }
}
} /// @note namespace fbzz::tests
