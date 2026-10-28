/// @file    WaterGridTests.cpp
/// @brief   カメラ近傍の水面密度・有限矩形・波の帯域と物理の基準密度を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Graphics/Effects/WaterGrid.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace fbzz::tests {
namespace {

constexpr uint32_t kOceanResolution = 512;
constexpr float kOceanExtent = 10000.0f;
constexpr float kNearCellSize = 2.0f;
constexpr float kPositionTolerance = testkit::kLooseTolerance;

class WaterGridTest : public testkit::Fixture {};

class WaterGridPersistenceTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
    }
    testkit::TempDir m_temp{ "water-grid" };
};

}

TEST_F(WaterGridTest, PreservesFiniteEndpointsWhenTheCameraIsInsideOrOutside)
{
    constexpr std::array<float, 5> focuses = { -20000.0f, -437.9f, 0.0f, 378.0f, 20000.0f };
    constexpr std::array<float, 2> extents = { kOceanExtent, 3600.0f };

    for (const float extent : extents) {
        for (const float focus : focuses) {
            const auto first = renderer::ResolveWaterGridAxis(0.0f, extent, kOceanResolution, focus, kNearCellSize);
            const auto last = renderer::ResolveWaterGridAxis(1.0f, extent, kOceanResolution, focus, kNearCellSize);

            EXPECT_FLOAT_EQ(first.position, -extent * 0.5f);
            EXPECT_FLOAT_EQ(last.position, extent * 0.5f);
        }
    }
}

TEST_F(WaterGridTest, KeepsAllCellsPositiveAcrossCameraPositions)
{
    constexpr std::array<float, 5> focuses = { -20000.0f, -4317.0f, 0.0f, 2741.0f, 20000.0f };

    for (const float focus : focuses) {
        float previous = -kOceanExtent * 0.5f;
        for (uint32_t vertex = 1; vertex <= kOceanResolution; ++vertex) {
            const float coordinate = static_cast<float>(vertex) / static_cast<float>(kOceanResolution);
            const auto sample = renderer::ResolveWaterGridAxis(coordinate, kOceanExtent, kOceanResolution, focus, kNearCellSize);

            EXPECT_TRUE(std::isfinite(sample.position));
            EXPECT_TRUE(std::isfinite(sample.cellSize));
            EXPECT_GT(sample.position, previous) << "focus " << focus << ", vertex " << vertex;
            EXPECT_GT(sample.cellSize, 0.0f);
            EXPECT_LE(sample.position, kOceanExtent * 0.5f);
            previous = sample.position;
        }
    }
}

TEST_F(WaterGridTest, CentersTheDenseBandOnTheSnappedCamera)
{
    constexpr float focus = 124.9f;

    const auto center = renderer::ResolveWaterGridAxis(0.5f, kOceanExtent, kOceanResolution, focus, kNearCellSize);
    const auto left = renderer::ResolveWaterGridAxis(0.375f, kOceanExtent, kOceanResolution, focus, kNearCellSize);
    const auto right = renderer::ResolveWaterGridAxis(0.625f, kOceanExtent, kOceanResolution, focus, kNearCellSize);

    EXPECT_FLOAT_EQ(center.position, 124.0f);
    EXPECT_VEC3_NEAR(math::Vector3(left.position, center.position, right.position),
                     math::Vector3(-4.0f, 124.0f, 252.0f), kPositionTolerance);
    EXPECT_VEC3_NEAR(math::Vector3(left.cellSize, center.cellSize, right.cellSize),
                     math::Vector3(kNearCellSize, kNearCellSize, kNearCellSize), kPositionTolerance);
}

TEST_F(WaterGridTest, HoldsTheGridStationaryForCameraMotionWithinASnapCell)
{
    constexpr float firstFocus = 124.1f;
    constexpr float secondFocus = 124.8f;

    for (uint32_t vertex = 0; vertex <= kOceanResolution; ++vertex) {
        const float coordinate = static_cast<float>(vertex) / static_cast<float>(kOceanResolution);
        const auto first = renderer::ResolveWaterGridAxis(coordinate, kOceanExtent, kOceanResolution, firstFocus, kNearCellSize);
        const auto second = renderer::ResolveWaterGridAxis(coordinate, kOceanExtent, kOceanResolution, secondFocus, kNearCellSize);

        EXPECT_FLOAT_EQ(first.position, second.position);
        EXPECT_FLOAT_EQ(first.cellSize, second.cellSize);
    }
}

TEST_F(WaterGridTest, KeepsTheDenseRegionInsideWaterWhenTheCameraIsOutside)
{
    constexpr float focus = 20000.0f;

    const auto center = renderer::ResolveWaterGridAxis(0.5f, kOceanExtent, kOceanResolution, focus, kNearCellSize);
    const auto nearby = renderer::ResolveWaterGridAxis(0.625f, kOceanExtent, kOceanResolution, focus, kNearCellSize);

    EXPECT_FLOAT_EQ(center.position, 4488.0f);
    EXPECT_FLOAT_EQ(nearby.position, 4616.0f);
    EXPECT_FLOAT_EQ(center.cellSize, kNearCellSize);
    EXPECT_FLOAT_EQ(nearby.cellSize, kNearCellSize);
}

TEST_F(WaterGridTest, PreservesUniformSpacingWhenTheWaterIsAlreadyFineEnough)
{
    constexpr float extent = 64.0f;
    constexpr uint32_t resolution = 64;

    for (uint32_t vertex = 0; vertex <= resolution; ++vertex) {
        const float coordinate = static_cast<float>(vertex) / static_cast<float>(resolution);
        const auto sample = renderer::ResolveWaterGridAxis(coordinate, extent, resolution, 20000.0f, kNearCellSize);

        EXPECT_FLOAT_EQ(sample.position, static_cast<float>(vertex) - extent * 0.5f);
        EXPECT_FLOAT_EQ(sample.cellSize, 1.0f);
    }
}

TEST_F(WaterGridTest, MeasuresTheLargestCellOnEitherSideOfEachVertex)
{
    std::array<float, kOceanResolution + 1> positions{};
    for (uint32_t vertex = 0; vertex <= kOceanResolution; ++vertex) {
        const float coordinate = static_cast<float>(vertex) / static_cast<float>(kOceanResolution);
        positions[vertex] = renderer::WaterGridAxisPosition(coordinate, kOceanExtent, kOceanResolution, 134.0f, kNearCellSize);
    }

    for (uint32_t vertex = 0; vertex <= kOceanResolution; ++vertex) {
        const float coordinate = static_cast<float>(vertex) / static_cast<float>(kOceanResolution);
        const auto sample = renderer::ResolveWaterGridAxis(coordinate, kOceanExtent, kOceanResolution, 134.0f, kNearCellSize);
        const float left = vertex > 0 ? positions[vertex] - positions[vertex - 1] : 0.0f;
        const float right = vertex < kOceanResolution ? positions[vertex + 1] - positions[vertex] : 0.0f;

        EXPECT_FLOAT_EQ(sample.position, positions[vertex]);
        EXPECT_FLOAT_EQ(sample.cellSize, (std::max)(left, right));
    }
}

TEST_F(WaterGridTest, RestoresOceanWavesNearTheCameraAndFiltersThemAtCoarseEdges)
{
    constexpr std::array<float, 4> wavelengths = { 9.0f, 14.0f, 18.0f, 22.0f };
    const auto near = renderer::ResolveWaterGridAxis(0.5f, kOceanExtent, kOceanResolution, 0.0f, kNearCellSize);
    const auto far = renderer::ResolveWaterGridAxis(0.0f, kOceanExtent, kOceanResolution, 0.0f, kNearCellSize);

    for (const float wavelength : wavelengths) {
        const float nearFade = scene::WaterComponent::WaveMeshFade(wavelength, { near.cellSize, near.cellSize });
        const float farFade = scene::WaterComponent::WaveMeshFade(wavelength, { far.cellSize, far.cellSize });

        EXPECT_FLOAT_EQ(nearFade, 1.0f);
        EXPECT_FLOAT_EQ(farFade, 0.0f);
    }
}

TEST_F(WaterGridTest, FadesWavesGraduallyAcrossTheDensityTransition)
{
    constexpr float wavelength = 9.0f;
    constexpr float maximumAdjacentFadeChange = 0.25f;
    bool hasPartialFade = false;
    float previousFade = 1.0f;

    for (uint32_t vertex = kOceanResolution / 2; vertex <= kOceanResolution; ++vertex) {
        const float coordinate = static_cast<float>(vertex) / static_cast<float>(kOceanResolution);
        const auto sample = renderer::ResolveWaterGridAxis(coordinate, kOceanExtent, kOceanResolution, 0.0f, kNearCellSize);
        const float fade = scene::WaterComponent::WaveMeshFade(wavelength, { sample.cellSize, sample.cellSize });

        EXPECT_LE(fade, previousFade);
        EXPECT_LE(previousFade - fade, maximumAdjacentFadeChange);
        hasPartialFade = hasPartialFade || (fade > 0.0f && fade < 1.0f);
        previousFade = fade;
    }
    EXPECT_TRUE(hasPartialFade);
    EXPECT_FLOAT_EQ(previousFade, 0.0f);
}

TEST_F(WaterGridTest, KeepsLegacyPhysicalDensityWhenCameraConcentrationIsDisabled)
{
    scene::WaterComponent water;
    water.extentX = kOceanExtent;
    water.extentZ = kOceanExtent;
    water.resolutionX = kOceanResolution;
    water.resolutionZ = kOceanResolution;
    scene::Transform transform;
    transform.worldScale = { 2.0f, 1.0f, 0.5f };

    const auto cell = scene::ResolveWaterCellSize(water, transform);

    EXPECT_FALSE(water.cameraFocusedGrid);
    EXPECT_VEC2_NEAR(cell, math::Vector2(39.0625f, 9.765625f), testkit::kTolerance);
}

TEST_F(WaterGridTest, ResolvesTheFocusedPhysicalDensityInWorldMeters)
{
    scene::WaterComponent water;
    water.extentX = kOceanExtent;
    water.extentZ = 64.0f;
    water.resolutionX = kOceanResolution;
    water.resolutionZ = 64;
    water.cameraFocusedGrid = true;
    water.nearCellSize = kNearCellSize;
    scene::Transform transform;
    transform.worldScale = { 2.0f, 1.0f, 0.5f };

    const auto cell = scene::ResolveWaterCellSize(water, transform);

    EXPECT_VEC2_NEAR(cell, math::Vector2(kNearCellSize, 0.5f), testkit::kTolerance);
}

TEST_F(WaterGridTest, MatchesTheDenseDisplayedGerstnerWaveAtDisplacedWorldPositions)
{
    scene::WaterComponent water;
    water.extentX = kOceanExtent;
    water.extentZ = kOceanExtent;
    water.resolutionX = kOceanResolution;
    water.resolutionZ = kOceanResolution;
    water.cameraFocusedGrid = true;
    water.nearCellSize = kNearCellSize;
    for (auto& wave : water.waves) wave.amplitude = 0.0f;
    water.waves[0] = { { 1.0f, 0.0f }, 0.35f, 14.0f, 0.2f };
    water.cellSize = scene::ResolveWaterCellSize(water, scene::Transform{});
    constexpr std::array<float, 3> coordinates = { 0.375f, 0.5f, 0.625f };
    constexpr std::array<float, 3> times = { 0.0f, 1.25f, 4.5f };
    const float waveNumber = math::TWO_PI / 14.0f;
    const float angularFrequency = std::sqrt(9.8f * waveNumber);

    for (const float coordinate : coordinates) {
        const auto sample = renderer::ResolveWaterGridAxis(coordinate, kOceanExtent, kOceanResolution, 124.0f, kNearCellSize);
        for (const float time : times) {
            const float phase = waveNumber * sample.position - angularFrequency * time;
            const float displayedX = sample.position + 0.2f * 0.35f * std::cos(phase);
            const float displayedHeight = 0.35f * std::sin(phase);
            const float physicalHeight = water.GetSurfaceHeightAt(displayedX, 0.0f, time);

            EXPECT_FLOAT_EQ(scene::WaterComponent::WaveMeshFade(14.0f, { sample.cellSize, sample.cellSize }), 1.0f);
            EXPECT_VEC3_NEAR(math::Vector3(displayedX, physicalHeight, 0.0f),
                             math::Vector3(displayedX, displayedHeight, 0.0f), testkit::kLooseTolerance);
        }
    }
}

TEST_F(WaterGridTest, FocusesHighViewsOnTheVisibleWaterIntersection)
{
    const auto world = math::Matrix4::Translate({ 0.0f, 19.0f, 0.0f });
    const math::Vector3 camera = { 0.0f, 220.0f, -1200.0f };
    const auto forward = math::Vector3(0.0f, -201.0f, 1100.0f).Normalized();

    const auto focus = renderer::ResolveWaterGridFocus(world, camera, forward,
        { static_cast<float>(kOceanResolution), static_cast<float>(kOceanResolution) }, kNearCellSize);

    EXPECT_VEC2_NEAR(focus, math::Vector2(0.0f, -100.0f), kPositionTolerance);
}

TEST_F(WaterGridTest, RetainsNearCameraDensityForViewsJustAboveSeaLevel)
{
    const auto world = math::Matrix4::Translate({ 0.0f, 19.0f, 0.0f });
    const math::Vector3 camera = { 0.0f, 25.0f, -1200.0f };
    const auto forward = math::Vector3(0.0f, -6.0f, 150.0f).Normalized();

    const auto focus = renderer::ResolveWaterGridFocus(world, camera, forward,
        { static_cast<float>(kOceanResolution), static_cast<float>(kOceanResolution) }, kNearCellSize);

    EXPECT_VEC2_NEAR(focus, math::Vector2(0.0f, -1192.96875f), kPositionTolerance);
}

TEST_F(WaterGridTest, LimitsFocusTravelWhenTheViewApproachesTheHorizon)
{
    const auto world = math::Matrix4::Translate({ 0.0f, 19.0f, 0.0f });
    const math::Vector3 camera = { 0.0f, 220.0f, -1200.0f };
    const auto forward = math::Vector3(0.0f, -1.0f, 100.0f).Normalized();

    const auto focus = renderer::ResolveWaterGridFocus(world, camera, forward,
        { static_cast<float>(kOceanResolution), static_cast<float>(kOceanResolution) }, kNearCellSize);

    EXPECT_VEC2_NEAR(focus, math::Vector2(0.0f, 408.0f), kPositionTolerance);
}

TEST_F(WaterGridTest, KeepsTheCameraFootprintWhenTheViewFacesAboveWater)
{
    const auto world = math::Matrix4::Translate({ 0.0f, 19.0f, 0.0f });
    const math::Vector3 camera = { 37.0f, 220.0f, -1200.0f };
    const auto forward = math::Vector3(0.0f, 1.0f, 100.0f).Normalized();

    const auto focus = renderer::ResolveWaterGridFocus(world, camera, forward,
        { static_cast<float>(kOceanResolution), static_cast<float>(kOceanResolution) }, kNearCellSize);

    EXPECT_VEC2_NEAR(focus, math::Vector2(37.0f, -1200.0f), kPositionTolerance);
}

TEST_F(WaterGridPersistenceTest, KeepsFocusedGridSettingsAcrossSceneSaveAndReload)
{
    scene::Scene source;
    auto& water = source.CreateGameObject("FocusedWater").AddComponent<scene::WaterComponent>();
    water.cameraFocusedGrid = true;
    water.nearCellSize = 3.0f;
    const auto path = m_temp.File("focused.scene").generic_string();

    ASSERT_TRUE(scene::SceneSerializer::Save(source, path));
    const auto restored = scene::SceneSerializer::LoadData(path);

    ASSERT_NE(restored, nullptr);
    auto* object = restored->Find("FocusedWater");
    ASSERT_NE(object, nullptr);
    const auto* reloaded = object->GetComponent<scene::WaterComponent>();
    ASSERT_NE(reloaded, nullptr);
    EXPECT_TRUE(reloaded->cameraFocusedGrid);
    EXPECT_FLOAT_EQ(reloaded->nearCellSize, 3.0f);
}

TEST_F(WaterGridPersistenceTest, LoadsLegacyScenesWithUniformGridDefaults)
{
    const auto path = m_temp.File("legacy.scene").generic_string();
    ASSERT_TRUE(util::FileSystem::WriteText(path,
        "[scene]\nformat_version = 2\n\n[[gameobjects]]\nname = 'LegacyWater'\n"
        "[gameobjects.WaterComponent]\nenabled = true\n"));

    const auto restored = scene::SceneSerializer::LoadData(path);

    ASSERT_NE(restored, nullptr);
    auto* object = restored->Find("LegacyWater");
    ASSERT_NE(object, nullptr);
    const auto* water = object->GetComponent<scene::WaterComponent>();
    ASSERT_NE(water, nullptr);
    EXPECT_FALSE(water->cameraFocusedGrid);
    EXPECT_FLOAT_EQ(water->nearCellSize, kNearCellSize);
    EXPECT_FALSE(water->adaptivePatchLod);
    EXPECT_EQ(water->surfaceShape, scene::WaterSurfaceShape::RECTANGLE);
    EXPECT_TRUE(water->splinePoints.empty());
}

TEST_F(WaterGridTest, PhysicalWaveHeightDoesNotDependOnMeshQuality)
{
    scene::WaterComponent water;
    for (auto& wave : water.waves) wave.amplitude = 0.0f;
    water.waves[0] = {{1.0f, 0.0f}, 0.5f, 4.0f, 0.0f};
    constexpr std::array<uint32_t, 3> resolutions{1u, 64u, 512u};
    for (const auto resolution : resolutions) {
        water.resolutionX = water.resolutionZ = resolution;
        water.cameraFocusedGrid = resolution != 1u;
        water.adaptivePatchLod = resolution == 512u;
        water.nearCellSize = 8.0f;
        water.cellSize = scene::ResolveWaterCellSize(water, scene::Transform{});
        EXPECT_NEAR(water.GetSurfaceHeightAt(1.0f, 0.0f, 0.0f), 0.5f, testkit::kTolerance);
    }
}

TEST_F(WaterGridTest, PhysicalRippleRemainsWhenTheMeshCannotResolveIt)
{
    scene::WaterComponent water;
    water.enableGerstnerWaves = false;
    scene::WaterRipple ripple;
    ripple.radius = 2.0f;
    ripple.width = 1.0f;
    ripple.amplitude = 0.8f;
    ripple.meshFade = 1.0f;
    water.ripples.push_back(ripple);
    const float height = water.GetSurfaceHeightAt(2.5f, 0.0f, 0.0f);
    ASSERT_GT(std::abs(height), 0.001f);
    water.ripples[0].meshFade = 0.0f;
    EXPECT_FLOAT_EQ(water.GetSurfaceHeightAt(2.5f, 0.0f, 0.0f), height);
    EXPECT_LE(std::abs(height), water.SurfaceHeightBound());
}

TEST_F(WaterGridPersistenceTest, KeepsAdaptivePatchLodSettingsAcrossSaveAndReload)
{
    scene::Scene source;
    auto& water = source.CreateGameObject("AdaptiveWater").AddComponent<scene::WaterComponent>();
    water.adaptivePatchLod = true;
    water.lodPatchResolution = 32;
    water.lodMaxDepth = 7;
    water.lodMaxPatches = 127;
    water.lodTargetPixels = 6.25f;
    water.lodNearDistance = 300.0f;
    water.lodNearCellSize = 1.5f;
    water.lodMorphEnabled = true;
    water.lodMorphTransitionWidth = 0.4f;
    water.waveAttenuationDepth = 3.5f;
    water.wavesAssetRef.path = "Assets/Data/OceanWaves.fzdata";
    water.farOceanEnabled = true;
    water.farOceanCoverageScale = 1.25f;
    water.farOceanRippleFadeDistance = 7.0f;
    const auto path = m_temp.File("adaptive.scene").generic_string();
    ASSERT_TRUE(scene::SceneSerializer::Save(source, path));
    const auto restored = scene::SceneSerializer::LoadData(path);
    ASSERT_NE(restored, nullptr);
    auto* object = restored->Find("AdaptiveWater");
    ASSERT_NE(object, nullptr);
    const auto* reloaded = object->GetComponent<scene::WaterComponent>();
    ASSERT_NE(reloaded, nullptr);
    EXPECT_TRUE(reloaded->adaptivePatchLod);
    EXPECT_EQ(reloaded->lodPatchResolution, 32u);
    EXPECT_EQ(reloaded->lodMaxDepth, 7u);
    EXPECT_EQ(reloaded->lodMaxPatches, 127u);
    EXPECT_FLOAT_EQ(reloaded->lodTargetPixels, 6.25f);
    EXPECT_FLOAT_EQ(reloaded->lodNearDistance, 300.0f);
    EXPECT_FLOAT_EQ(reloaded->lodNearCellSize, 1.5f);
    EXPECT_TRUE(reloaded->lodMorphEnabled);
    EXPECT_FLOAT_EQ(reloaded->lodMorphTransitionWidth, 0.4f);
    EXPECT_FLOAT_EQ(reloaded->waveAttenuationDepth, 3.5f);
    EXPECT_EQ(reloaded->wavesAssetRef.path, "Assets/Data/OceanWaves.fzdata");
    EXPECT_EQ(reloaded->wavesAssetRef.type, "WaterWavesAsset");
    EXPECT_TRUE(reloaded->farOceanEnabled);
    EXPECT_FLOAT_EQ(reloaded->farOceanCoverageScale, 1.25f);
    EXPECT_FLOAT_EQ(reloaded->farOceanRippleFadeDistance, 7.0f);
}

TEST_F(WaterGridPersistenceTest, KeepsSplineRiverPointIdsDimensionsDepthAndFlowAcrossSaveAndReload)
{
    scene::Scene source;
    auto& water = source.CreateGameObject("SplineRiver").AddComponent<scene::WaterComponent>();
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.splineSamplesPerSegment = 12;
    water.splineMeshCellSize = 1.25f;
    water.splinePoints = {
        {4, {-10.0f, 3.0f, 0.0f}, 6.0f, 1.5f, 2.0f},
        {7, {0.0f, 2.0f, 3.0f}, 8.0f, 2.5f, -1.0f},
        {scene::WATER_SPLINE_MAX_POINT_ID, {10.0f, 1.0f, 0.0f}, 4.0f, 3.0f, 0.0f}};
    const auto path = m_temp.File("spline-river.scene").generic_string();
    ASSERT_TRUE(scene::SceneSerializer::Save(source, path));
    const auto restored = scene::SceneSerializer::LoadData(path);
    ASSERT_NE(restored, nullptr);
    auto* object = restored->Find("SplineRiver");
    ASSERT_NE(object, nullptr);
    const auto* loaded = object->GetComponent<scene::WaterComponent>();
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->surfaceShape, scene::WaterSurfaceShape::SPLINE_RIVER);
    EXPECT_EQ(loaded->splineSamplesPerSegment, 12u);
    EXPECT_FLOAT_EQ(loaded->splineMeshCellSize, 1.25f);
    ASSERT_EQ(loaded->splinePoints.size(), water.splinePoints.size());
    for (size_t i = 0; i < water.splinePoints.size(); ++i) {
        const auto& expected = water.splinePoints[i];
        const auto& actual = loaded->splinePoints[i];
        EXPECT_EQ(actual.id, expected.id);
        EXPECT_VEC3_NEAR(actual.localPosition, expected.localPosition, testkit::kTolerance);
        EXPECT_FLOAT_EQ(actual.width, expected.width);
        EXPECT_FLOAT_EQ(actual.depth, expected.depth);
        EXPECT_FLOAT_EQ(actual.flowSpeed, expected.flowSpeed);
    }
    EXPECT_EQ(loaded->splineSnapshot, nullptr);
}
} /// @note namespace fbzz::tests
