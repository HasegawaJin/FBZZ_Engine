/// @file    WaterSurfaceQueryTests.cpp
/// @brief   水面照会の world TRS・有限水深・変位後境界と共通出力を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Fields/FlowFieldEval.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/WaterExclusion.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Math/MathUtils.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <memory>

namespace fbzz::tests {
namespace {
scene::WaterComponent FlatWater()
{
    scene::WaterComponent water;
    for (auto& wave : water.waves) wave.amplitude = 0.0f;
    water.extentX = 20.0f;
    water.extentZ = 4.0f;
    water.buoyancyDepth = 5.0f;
    return water;
}

math::Vector3 WorldPoint(const scene::Transform& pose, const math::Vector3& local)
{
    return pose.worldPosition + pose.worldRotation * math::Vector3{
        local.x * pose.worldScale.x, local.y * pose.worldScale.y, local.z * pose.worldScale.z};
}

void ExpectUnchanged(const scene::WaterSurfaceHeightSample& sample)
{
    EXPECT_VEC3_NEAR(sample.position, math::Vector3(1.0f, 2.0f, 3.0f), testkit::kTolerance);
    EXPECT_FLOAT_EQ(sample.depth, 7.0f);
    EXPECT_TRUE(sample.insideSurface);
    EXPECT_TRUE(sample.underwater);
    EXPECT_FLOAT_EQ(sample.displacement, 11.0f);
}
} /// @note namespace

class WaterSurfaceQueryTest : public testkit::Fixture {};

TEST_F(WaterSurfaceQueryTest, HorizonToggleExtendsMembershipButPreservesHeightAndDepth)
{
    auto water = FlatWater();
    scene::Transform pose;
    pose.worldPosition = {100.0f, 12.0f, -40.0f};
    pose.worldScale = {-2.0f, 3.0f, 0.5f};
    pose.worldRotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(45.0f));
    scene::WaterSurfaceHeightSample sample;
    for (bool horizon : {false, true, false, true}) {
        water.farOceanEnabled = horizon;
        for (const auto& local : {math::Vector3{0, 0, 0}, math::Vector3{360, 0, 360}, math::Vector3{-360, 0, -360}}) {
            auto point = WorldPoint(pose, local);
            point.y = 11.0f;
            const bool expectedInside = horizon || local.x == 0.0f;
            ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, point, 0.0f, sample));
            EXPECT_EQ(sample.insideSurface, expectedInside);
            EXPECT_EQ(sample.underwater, expectedInside);
            EXPECT_EQ(scene::WaterSurfaceContainsXZ(water, pose, point.x, point.z, 0.0f), expectedInside);
            EXPECT_FLOAT_EQ(sample.position.y, 12.0f);
            EXPECT_FLOAT_EQ(sample.waterDepthLimit, 5.0f);
            for (float height : {6.9f, 12.0f, 13.0f}) {
                point.y = height;
                ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, point, 0.0f, sample));
                EXPECT_FALSE(sample.underwater);
            }
        }
    }
}

TEST_F(WaterSurfaceQueryTest, HorizonRaisesCrossingCrestsWithoutRaisingAlignedCrestsOrTroughs)
{
    auto water = FlatWater();
    water.waveCount = 2;
    water.waves[0] = {{1.0f, 0.0f}, 1.0f, 8.0f, 0.0f, math::PI * 0.5f};
    water.waves[1] = water.waves[0];
    scene::Transform pose;
    scene::WaterSurfaceHeightSample sample;

    water.farOceanEnabled = true;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {0, 0, 0}, 0.0f, sample));
    EXPECT_NEAR(sample.displacement, 2.0f, testkit::kTolerance);

    water.waves[1].direction = {0.0f, 1.0f};
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {0, 0, 0}, 0.0f, sample));
    EXPECT_NEAR(sample.displacement, 2.85f, testkit::kTolerance);
    EXPECT_LE(sample.displacement, water.SurfaceHeightBound());
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {4, 0, 4}, 0.0f, sample));
    EXPECT_NEAR(sample.displacement, -2.0f, testkit::kTolerance);

    water.farOceanEnabled = false;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {0, 0, 0}, 0.0f, sample));
    EXPECT_NEAR(sample.displacement, 2.0f, testkit::kTolerance);
}

TEST_F(WaterSurfaceQueryTest, WaveSpeedUpdatesTheCachedSurfacePhase)
{
    auto water = FlatWater();
    water.waveCount = 1;
    water.waves[0] = {{1.0f, 0.0f}, 1.0f, 12.0f, 0.2f, 0.0f};
    const scene::Transform pose;
    const math::Vector3 point{1.5f, 0.0f, 0.5f};
    scene::WaterSurfaceHeightSample initial;
    scene::WaterSurfaceHeightSample advanced;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, point, 0.0f, initial));
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, point, 0.8f, advanced));
    ASSERT_GT(std::abs(initial.displacement - advanced.displacement), 0.1f);

    scene::WaterSurfaceHeightSample sample;
    water.waveAuthoring.speedMultiplier = 2.0f;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, point, 0.4f, sample));
    EXPECT_NEAR(sample.displacement, advanced.displacement, testkit::kTolerance);

    water.waveAuthoring.speedMultiplier = 0.0f;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, point, 0.8f, sample));
    EXPECT_NEAR(sample.displacement, initial.displacement, testkit::kTolerance);
}

TEST_F(WaterSurfaceQueryTest, UsesWorldParentPoseYawAndSignedNonuniformScale)
{
    const auto water = FlatWater();
    scene::Transform pose;
    pose.position = {-900.0f, -300.0f, 400.0f};
    pose.worldPosition = {100.0f, 12.0f, -40.0f};
    pose.worldScale = {-2.0f, 3.0f, 0.5f};
    pose.worldRotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(45.0f));
    auto inside = WorldPoint(pose, {9.5f, 0.0f, 1.5f});
    inside.y -= 1.0f;
    scene::WaterSurfaceHeightSample sample;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, inside, 0.0f, sample));
    EXPECT_TRUE(sample.insideSurface);
    EXPECT_TRUE(sample.underwater);
    EXPECT_FLOAT_EQ(sample.position.y, 12.0f);
    EXPECT_FLOAT_EQ(sample.depth, 1.0f);
    auto outside = WorldPoint(pose, {9.5f, 0.0f, 2.5f});
    outside.y -= 1.0f;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, outside, 0.0f, sample));
    EXPECT_FALSE(sample.insideSurface);
    EXPECT_FALSE(sample.underwater);
    EXPECT_FLOAT_EQ(sample.position.y, 12.0f);
}

TEST_F(WaterSurfaceQueryTest, LimitsUnderwaterToTheAuthoredDepthAndExcludesTheWaterline)
{
    const auto water = FlatWater();
    scene::Transform pose;
    pose.worldPosition.y = 12.0f;
    scene::WaterSurfaceHeightSample sample;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {0.0f, 7.0f, 0.0f}, 0.0f, sample));
    EXPECT_TRUE(sample.underwater);
    EXPECT_FLOAT_EQ(sample.depth, water.buoyancyDepth);
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {0.0f, 6.9f, 0.0f}, 0.0f, sample));
    EXPECT_FALSE(sample.underwater);
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {0.0f, 12.0f, 0.0f}, 0.0f, sample));
    EXPECT_FALSE(sample.underwater);
    EXPECT_FLOAT_EQ(sample.depth, 0.0f);
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {0.0f, 13.0f, 0.0f}, 0.0f, sample));
    EXPECT_FALSE(sample.underwater);
    EXPECT_FLOAT_EQ(sample.depth, -1.0f);
}

TEST_F(WaterSurfaceQueryTest, InsideOnlyHeightMatchesTheSplineQueryAndPreservesOutputOnMiss)
{
    auto water = FlatWater();
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.splineSamplesPerSegment = 4;
    water.splineMeshCellSize = 100.0f;
    water.waveCount = 1;
    water.waves[0] = {{1.0f, 0.0f}, 0.25f, 20.0f, 0.5f};
    water.splinePoints.resize(2);
    water.splinePoints[0].id = 1;
    water.splinePoints[0].width = 4.0f;
    water.splinePoints[0].depth = 2.0f;
    water.splinePoints[1].id = 2;
    water.splinePoints[1].localPosition = {10.0f, 2.0f, 0.0f};
    water.splinePoints[1].width = 4.0f;
    water.splinePoints[1].depth = 5.0f;

    scene::Transform pose;
    pose.worldPosition = {100.0f, 50.0f, -20.0f};
    pose.worldScale = {-2.0f, 3.0f, 0.5f};
    ASSERT_TRUE(scene::PrepareWaterSplineSnapshot(water, pose));
    const math::Vector3 inside{90.0f, 54.0f, -20.0f};
    constexpr float TIME = 0.25f;

    scene::WaterSurfaceHeightSample full, filtered;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, inside, TIME, full));
    ASSERT_TRUE(full.insideSurface);
    ASSERT_FALSE(full.underwater);
    ASSERT_TRUE(scene::TrySampleInsideWaterSurfaceHeightWorld(water, pose, inside, TIME, filtered));
    EXPECT_EQ(filtered.position.x, full.position.x);
    EXPECT_EQ(filtered.position.y, full.position.y);
    EXPECT_EQ(filtered.position.z, full.position.z);
    EXPECT_EQ(filtered.depth, full.depth);
    EXPECT_EQ(filtered.displacement, full.displacement);
    EXPECT_EQ(filtered.waterDepthLimit, full.waterDepthLimit);
    EXPECT_EQ(filtered.baseHeightOffset, full.baseHeightOffset);
    EXPECT_EQ(filtered.baseHeightGradient.x, full.baseHeightGradient.x);
    EXPECT_EQ(filtered.baseHeightGradient.y, full.baseHeightGradient.y);
    EXPECT_TRUE(filtered.insideSurface);
    EXPECT_FALSE(filtered.excluded);
    EXPECT_FALSE(filtered.underwater);

    filtered = {{1.0f, 2.0f, 3.0f}, 7.0f, true, true, 11.0f};
    const math::Vector3 outside{90.0f, 54.0f, -24.0f};
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, outside, TIME, full));
    ASSERT_FALSE(full.insideSurface);
    EXPECT_FALSE(scene::TrySampleInsideWaterSurfaceHeightWorld(water, pose, outside, TIME, filtered));
    ExpectUnchanged(filtered);

    auto exclusions = std::make_shared<scene::WaterExclusionSnapshot>();
    scene::WaterExclusionBox box;
    box.worldPosition = inside;
    box.halfExtent = math::Vector3::ONE;
    exclusions->boxes.push_back(box);
    water.exclusionSnapshot = exclusions;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, inside, TIME, full));
    ASSERT_TRUE(full.insideSurface);
    ASSERT_TRUE(full.excluded);
    EXPECT_FALSE(scene::TrySampleInsideWaterSurfaceHeightWorld(water, pose, inside, TIME, filtered));
    ExpectUnchanged(filtered);
}

TEST_F(WaterSurfaceQueryTest, AcceptsTheHorizontalUpsideDownPlane)
{
    const auto water = FlatWater();
    scene::Transform pose;
    pose.worldPosition = {5.0f, 3.0f, 7.0f};
    pose.worldScale = {2.0f, -3.0f, -0.5f};
    pose.worldRotation = math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, math::PI);
    EXPECT_TRUE(scene::IsWaterSurfaceSupported(water, pose));
    scene::WaterSurfaceSample sample;
    auto point = WorldPoint(pose, {3.0f, 0.0f, 1.0f});
    point.y = 2.0f;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceWorld(water, pose, {}, point, 0.0f, sample));
    EXPECT_TRUE(sample.height.underwater);
    EXPECT_VEC3_NEAR(sample.normal, math::Vector3::UP, testkit::kTolerance);
}

TEST_F(WaterSurfaceQueryTest, RejectsTiltedSingularAndNonfiniteInputsWithoutChangingOutput)
{
    auto water = FlatWater();
    scene::Transform pose;
    scene::WaterSurfaceHeightSample sample{{1.0f, 2.0f, 3.0f}, 7.0f, true, true, 11.0f};
    pose.worldRotation = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, math::ToRad(20.0f));
    EXPECT_FALSE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
    ExpectUnchanged(sample);
    pose.worldRotation = math::Quaternion::Identity();
    pose.worldScale.x = 0.0f;
    EXPECT_FALSE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
    ExpectUnchanged(sample);
    pose.worldScale = math::Vector3::ONE;
    pose.worldRotation.w = 2.0f;
    EXPECT_FALSE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
    ExpectUnchanged(sample);
    pose.worldRotation = math::Quaternion::Identity();
    const float invalid = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {invalid, 0.0f, 0.0f}, 0.0f, sample));
    ExpectUnchanged(sample);
    EXPECT_FALSE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, invalid, sample));
    ExpectUnchanged(sample);
    water.extentX = invalid;
    EXPECT_FALSE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
    ExpectUnchanged(sample);
}

TEST_F(WaterSurfaceQueryTest, RejectsInvalidWorldExtentDepthAndResolvedWaves)
{
    auto water = FlatWater();
    scene::Transform pose;
    water.extentX = 0.0f;
    EXPECT_FALSE(scene::IsWaterSurfaceSupported(water, pose));
    water.extentX = 20.0f;
    water.buoyancyDepth = -1.0f;
    EXPECT_FALSE(scene::IsWaterSurfaceSupported(water, pose));
    water.buoyancyDepth = 5.0f;
    pose.worldScale.x = std::numeric_limits<float>::max();
    EXPECT_FALSE(scene::IsWaterSurfaceSupported(water, pose));
    pose.worldScale.x = 1.0f;
    water.waves[0].direction.x = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(scene::IsWaterSurfaceSupported(water, pose));
}

TEST_F(WaterSurfaceQueryTest, RejectsDegenerateAndOverflowingActiveWaveDirections)
{
    auto water = FlatWater();
    water.waves[0] = {{0.0f, 0.0f}, 0.5f, 10.0f, 0.0f};
    scene::Transform pose;
    scene::WaterSurfaceHeightSample sample{{1.0f, 2.0f, 3.0f}, 7.0f, true, true, 11.0f};
    EXPECT_FALSE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
    ExpectUnchanged(sample);
    water.waves[0].direction = {0.00000001f, 0.0f};
    EXPECT_FALSE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
    ExpectUnchanged(sample);
    water.waves[0].direction = {1.0e20f, 0.0f};
    EXPECT_FALSE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
    ExpectUnchanged(sample);
    water.waves[0].direction = {1.0f, 0.0f};
    EXPECT_TRUE(scene::IsWaterSurfaceSupported(water, pose));
}

TEST_F(WaterSurfaceQueryTest, InactiveWavesDoNotRequireADirection)
{
    auto water = FlatWater();
    water.waves[0].direction = math::Vector2::ZERO;
    scene::Transform pose;
    scene::WaterSurfaceHeightSample sample;
    EXPECT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
    water.waves[0].amplitude = 0.5f;
    water.enableGerstnerWaves = false;
    EXPECT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
    EXPECT_FLOAT_EQ(sample.position.y, 0.0f);
}

TEST_F(WaterSurfaceQueryTest, UsesWorldWaveHeightNormalAndLocalFlowWithoutVerticalScale)
{
    auto water = FlatWater();
    water.waves[0] = {{1.0f, 0.0f}, 0.5f, 10.0f, 0.0f};
    water.current = {1.0f, 2.0f};
    scene::Transform pose;
    pose.worldPosition.y = 3.0f;
    pose.worldScale.y = 128.0f;
    scene::ActiveFlowField field{};
    field.type = scene::FlowFieldType::Uniform;
    field.direction = {0.6f, 0.8f, 0.0f};
    field.strength = 5.0f;
    field.radius = 0.0f;
    field.channels = 0xffffffffu;
    scene::WaterSurfaceSample sample;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceWorld(water, pose, {field}, {0.0f, 2.0f, 0.0f}, 0.0f, sample));
    EXPECT_FLOAT_EQ(sample.height.position.y, 3.0f);
    const math::Vector3 expected = math::Vector3{-0.5f * math::TWO_PI / 10.0f, 1.0f, 0.0f}.Normalized();
    EXPECT_VEC3_NEAR(sample.normal, expected, 0.0001f);
    EXPECT_VEC3_NEAR(sample.flowVelocity, math::Vector3(4.0f, 0.0f, 2.0f), testkit::kTolerance);
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {2.5f, 2.0f, 0.0f}, 0.0f, sample.height));
    EXPECT_NEAR(sample.height.position.y, 3.5f, testkit::kTolerance);
}

TEST_F(WaterSurfaceQueryTest, ClassifiesTheDisplacedBoundaryThroughTheGerstnerInverse)
{
    auto water = FlatWater();
    water.extentX = 2.0f;
    water.waves[0] = {{1.0f, 0.0f}, 0.5f, 20.0f, 0.5f};
    scene::Transform pose;
    const auto displacedX = [&](float baseX) {
        return baseX + 0.25f * std::cos(math::TWO_PI * baseX / 20.0f);
    };
    EXPECT_GT(displacedX(0.99f), water.extentX * 0.5f);
    EXPECT_TRUE(scene::WaterSurfaceContainsXZ(water, pose, displacedX(0.99f), 0.0f, 0.0f));
    EXPECT_FALSE(scene::WaterSurfaceContainsXZ(water, pose, displacedX(1.1f), 0.0f, 0.0f));
}

TEST_F(WaterSurfaceQueryTest, FullQueryKeepsOutputWhenTheFlowIsInvalid)
{
    auto water = FlatWater();
    water.current.y = std::numeric_limits<float>::quiet_NaN();
    scene::Transform pose;
    scene::WaterSurfaceSample sample;
    sample.height = {{1.0f, 2.0f, 3.0f}, 7.0f, true, true, 11.0f};
    sample.normal = math::Vector3::RIGHT;
    sample.flowVelocity = {3.0f, 4.0f, 5.0f};
    EXPECT_FALSE(scene::TrySampleWaterSurfaceWorld(water, pose, {}, {}, 0.0f, sample));
    ExpectUnchanged(sample.height);
    EXPECT_VEC3_NEAR(sample.normal, math::Vector3::RIGHT, testkit::kTolerance);
    EXPECT_VEC3_NEAR(sample.flowVelocity, math::Vector3(3.0f, 4.0f, 5.0f), testkit::kTolerance);
}

TEST_F(WaterSurfaceQueryTest, PreservesTheRawDisplacementAtLargeWorldHeight)
{
    auto water = FlatWater();
    water.waves[0] = {{1.0f, 0.0f}, 0.5f, 10.0f, 0.0f};
    scene::Transform pose;
    pose.worldPosition.y = 1.0e8f;
    scene::WaterSurfaceHeightSample sample;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {2.5f, pose.worldPosition.y, 0.0f}, 0.0f, sample));
    EXPECT_FLOAT_EQ(sample.position.y, pose.worldPosition.y);
    EXPECT_NEAR(sample.displacement, 0.5f, testkit::kTolerance);
}

TEST_F(WaterSurfaceQueryTest, ResolvesANonflatNormalWhenTheWorldCoordinateUlpExceedsTheDifferenceStep)
{
    auto water = FlatWater();
    water.waves[0] = {{1.0f, 0.0f}, 10.0f, 1000000.0f, 0.0f};
    constexpr float worldX = 2000000.0f;
    scene::Transform pose;
    pose.worldPosition = {worldX, 1.0e8f, 0.0f};
    scene::WaterSurfaceSample sample;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceWorld(water, pose, {}, pose.worldPosition, 0.0f, sample));
    const float waveNumber = math::TWO_PI / water.waves[0].wavelength;
    const float slope = water.waves[0].amplitude * waveNumber * std::cos(waveNumber * worldX);
    const auto expected = math::Vector3{-slope, 1.0f, 0.0f}.Normalized();
    EXPECT_LT(sample.normal.x, -0.00004f);
    EXPECT_VEC3_NEAR(sample.normal, expected, 0.00002f);
    EXPECT_FLOAT_EQ(sample.height.position.y, pose.worldPosition.y);
    pose.worldPosition.x = std::numeric_limits<float>::max();
    sample.height = {{1.0f, 2.0f, 3.0f}, 7.0f, true, true, 11.0f};
    sample.normal = math::Vector3::RIGHT;
    sample.flowVelocity = {3.0f, 4.0f, 5.0f};
    EXPECT_FALSE(scene::TrySampleWaterSurfaceWorld(water, pose, {}, pose.worldPosition, 0.0f, sample));
    ExpectUnchanged(sample.height);
    EXPECT_VEC3_NEAR(sample.normal, math::Vector3::RIGHT, testkit::kTolerance);
    EXPECT_VEC3_NEAR(sample.flowVelocity, math::Vector3(3.0f, 4.0f, 5.0f), testkit::kTolerance);
}

TEST_F(WaterSurfaceQueryTest, RejectsScaleBelowTheSupportedMagnitudeAndAcceptsItsSignedBoundary)
{
    const auto water = FlatWater();
    scene::Transform pose;
    const std::array<float*, 3> axes{&pose.worldScale.x, &pose.worldScale.y, &pose.worldScale.z};
    const float below = std::nextafter(0.0001f, 0.0f);
    for (float* axis : axes) {
        scene::WaterSurfaceHeightSample sample{{1.0f, 2.0f, 3.0f}, 7.0f, true, true, 11.0f};
        *axis = below;
        EXPECT_FALSE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
        ExpectUnchanged(sample);
        *axis = -below;
        EXPECT_FALSE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
        ExpectUnchanged(sample);
        *axis = 0.0001f;
        EXPECT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
        *axis = -0.0001f;
        EXPECT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
        *axis = 1.0f;
    }
}

TEST_F(WaterSurfaceQueryTest, RejectsNonfiniteLiveFlowShapeAndPreservesOutput)
{
    auto water = FlatWater();
    water.surfaceFlowCount = 1;
    auto& flow = water.surfaceFlows[0];
    flow.kind = scene::FlowFieldType::Uniform;
    flow.radius = 4.0f;
    flow.height = 0.0f;
    flow.speed = 1.0f;
    flow.chop = 0.3f;
    flow.direction = {1.0f, 0.0f};
    const std::array<float*, 9> inputs{&flow.center.x, &flow.center.y, &flow.radius, &flow.height,
        &flow.speed, &flow.falloffPower, &flow.chop, &flow.direction.x, &flow.direction.y};
    scene::Transform pose;
    scene::WaterSurfaceHeightSample sample{{1.0f, 2.0f, 3.0f}, 7.0f, true, true, 11.0f};
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        for (float* input : inputs) {
            const float original = *input;
            *input = invalid;
            EXPECT_FALSE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
            ExpectUnchanged(sample);
            *input = original;
        }
    }
}

TEST_F(WaterSurfaceQueryTest, RejectsNonfiniteLiveRippleShapeAndPreservesOutput)
{
    auto water = FlatWater();
    water.ripples.push_back({});
    auto& ripple = water.ripples.back();
    ripple.amplitude = 1.0f;
    const std::array<float*, 9> inputs{&ripple.center.x, &ripple.center.y, &ripple.radius, &ripple.width,
        &ripple.amplitude, &ripple.speed, &ripple.decayRate, &ripple.meshFade, &ripple.maxRadius};
    scene::Transform pose;
    scene::WaterSurfaceHeightSample sample{{1.0f, 2.0f, 3.0f}, 7.0f, true, true, 11.0f};
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        for (float* input : inputs) {
            const float original = *input;
            *input = invalid;
            EXPECT_FALSE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
            ExpectUnchanged(sample);
            *input = original;
        }
    }
}

TEST_F(WaterSurfaceQueryTest, RejectsInvalidBakedVolumeShapeBeforeSampling)
{
    auto water = FlatWater();
    fluid::VectorFieldAsset asset;
    asset.sizeX = asset.sizeY = asset.sizeZ = 2;
    asset.data.assign(8, math::Vector3{1.0f, 0.0f, 0.0f});
    water.surfaceFlowCount = 1;
    auto& flow = water.surfaceFlows[0];
    flow.kind = scene::FlowFieldType::Baked;
    flow.vectorField = &asset;
    flow.height = -0.5f;
    flow.speed = 1.0f;
    const std::array<float*, 10> inputs{&flow.center.x, &flow.center.y, &flow.extents.x,
        &flow.extents.y, &flow.extents.z, &flow.inverseRotation.w, &flow.planeOffsetY,
        &flow.speed, &asset.boundsMin.x, &asset.maxMagnitude};
    scene::Transform pose;
    scene::WaterSurfaceHeightSample sample{{1.0f, 2.0f, 3.0f}, 7.0f, true, true, 11.0f};
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        for (float* input : inputs) {
            const float original = *input;
            *input = invalid;
            EXPECT_FALSE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
            ExpectUnchanged(sample);
            *input = original;
        }
    }
    flow.extents.x = 0.0f;
    EXPECT_FALSE(scene::IsWaterSurfaceSupported(water, pose));
}

TEST_F(WaterSurfaceQueryTest, IgnoresUnusedFlowSlotsInactiveRipplesAndShapeParameters)
{
    auto water = FlatWater();
    const float invalid = std::numeric_limits<float>::quiet_NaN();
    water.surfaceFlows[7].height = invalid;
    water.ripples.push_back({});
    water.ripples.back().center.x = invalid;
    water.ripples.back().width = invalid;
    water.surfaceFlowCount = 1;
    auto& flow = water.surfaceFlows[0];
    flow.kind = scene::FlowFieldType::Uniform;
    flow.radius = 4.0f;
    flow.direction = {1.0f, 0.0f};
    flow.extents.x = invalid;
    flow.inverseRotation.w = invalid;
    flow.planeOffsetY = invalid;
    scene::Transform pose;
    scene::WaterSurfaceHeightSample sample;
    EXPECT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
    flow.radius = 0.0f;
    flow.center.x = invalid;
    flow.speed = invalid;
    flow.falloffPower = invalid;
    flow.chop = invalid;
    flow.direction.x = invalid;
    EXPECT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
}

TEST_F(WaterSurfaceQueryTest, RejectsNonfiniteWaterDepthCurrentAndActiveBuoyancy)
{
    auto water = FlatWater();
    scene::Transform pose;
    const std::array<float*, 5> inputs{&water.buoyancyDepth, &water.current.x, &water.current.y,
        &water.buoyancy, &water.waterDrag};
    scene::WaterSurfaceHeightSample sample{{1.0f, 2.0f, 3.0f}, 7.0f, true, true, 11.0f};
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        for (float* input : inputs) {
            const float original = *input;
            *input = invalid;
            EXPECT_FALSE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
            ExpectUnchanged(sample);
            *input = original;
        }
    }
    water.buoyancyEnabled = false;
    water.buoyancy = water.waterDrag = std::numeric_limits<float>::quiet_NaN();
    EXPECT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
}

TEST_F(WaterSurfaceQueryTest, RejectsNonfiniteFlowQueriesAndIgnoresZeroOrMaskedFields)
{
    const auto water = FlatWater();
    scene::Transform pose;
    scene::ActiveFlowField field{};
    field.type = scene::FlowFieldType::Uniform;
    field.direction = math::Vector3::RIGHT;
    field.channels = 0xffffffffu;
    field.strength = 1.0f;
    scene::WaterSurfaceSample sample;
    sample.height = {{1.0f, 2.0f, 3.0f}, 7.0f, true, true, 11.0f};
    sample.normal = math::Vector3::RIGHT;
    sample.flowVelocity = {3.0f, 4.0f, 5.0f};
    const std::array<float*, 3> inputs{&field.strength, &field.radius, &field.direction.x};
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        for (float* input : inputs) {
            const float original = *input;
            *input = invalid;
            EXPECT_FALSE(scene::TrySampleWaterSurfaceWorld(water, pose, {field}, {}, 0.0f, sample));
            ExpectUnchanged(sample.height);
            EXPECT_VEC3_NEAR(sample.normal, math::Vector3::RIGHT, testkit::kTolerance);
            EXPECT_VEC3_NEAR(sample.flowVelocity, math::Vector3(3.0f, 4.0f, 5.0f), testkit::kTolerance);
            *input = original;
        }
    }
    field.channels = 0;
    field.strength = std::numeric_limits<float>::quiet_NaN();
    EXPECT_TRUE(scene::TrySampleWaterSurfaceWorld(water, pose, {field}, {}, 0.0f, sample));
    field.channels = 0xffffffffu;
    field.strength = 0.0f;
    field.radius = field.direction.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_TRUE(scene::TrySampleWaterSurfaceWorld(water, pose, {field}, {}, 0.0f, sample));
    EXPECT_VEC3_NEAR(sample.flowVelocity, math::Vector3::ZERO, testkit::kTolerance);
}
} /// @note namespace fbzz::tests
