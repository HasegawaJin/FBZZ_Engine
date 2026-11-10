/// @file    WaterDepthFieldTests.cpp
/// @brief   浅水波減衰の符号、勾配、変換、CPU snapshot を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/WaterZoneComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/WaterDepthField.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Engine/Scene/WaterTerrainInfo.hpp>
#include <Math/MathUtils.hpp>
#include <Physics/World.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace fbzz::tests {
namespace {
scene::WaterComponent MakeWater()
{
    scene::WaterComponent water;
    water.extentX = water.extentZ = 2.0f;
    water.buoyancyDepth = 2.0f;
    water.waveAttenuationDepth = 2.0f;
    water.waveCount = 0;
    return water;
}

uint16_t EncodeDepth(float depth, float range)
{
    const float normalized = std::clamp(depth / range * 0.5f + 0.5f, 0.0f, 1.0f);
    return static_cast<uint16_t>(std::round(normalized * 65535.0f));
}

scene::WaterTerrainInfoAtlas MakeAtlas(const std::array<float, 4>& depths, uint8_t flags, float range = 4.0f)
{
    scene::WaterTerrainInfoAtlas atlas;
    atlas.width = atlas.height = 2;
    atlas.depthRange = range;
    atlas.signature = 91;
    atlas.rgba.resize(16);
    for (size_t i = 0; i < depths.size(); ++i) {
        const uint16_t word = EncodeDepth(depths[i], range);
        atlas.rgba[i * 4u] = 0;
        atlas.rgba[i * 4u + 1] = static_cast<uint8_t>(word >> 8u);
        atlas.rgba[i * 4u + 2] = static_cast<uint8_t>(word & 255u);
        atlas.rgba[i * 4u + 3] = flags;
    }
    return atlas;
}

scene::WaterTerrainInfoAtlas MakeSolidAtlas(uint32_t width, uint32_t height, float depth,
    uint8_t flags, float range)
{
    scene::WaterTerrainInfoAtlas atlas;
    atlas.width = width;
    atlas.height = height;
    atlas.depthRange = range;
    atlas.signature = 92;
    atlas.rgba.resize(static_cast<size_t>(width) * height * 4u);
    const uint16_t word = EncodeDepth(depth, range);
    for (size_t pixel = 0; pixel < static_cast<size_t>(width) * height; ++pixel) {
        atlas.rgba[pixel * 4u] = 0;
        atlas.rgba[pixel * 4u + 1] = static_cast<uint8_t>(word >> 8u);
        atlas.rgba[pixel * 4u + 2] = static_cast<uint8_t>(word & 255u);
        atlas.rgba[pixel * 4u + 3] = flags;
    }
    return atlas;
}

scene::GameObject& AddTerrain(scene::Scene& source, float height)
{
    auto& object = source.CreateGameObject("Terrain");
    auto& terrain = object.AddComponent<scene::TerrainComponent>();
    terrain.columns = terrain.rows = 9;
    terrain.cellSize = terrain.maxHeight = 1.0f;
    terrain.InitFlat(height);
    object.transform.position = {-4.0f, 0.0f, -4.0f};
    return object;
}

scene::WaterComponent& AddWater(scene::Scene& source)
{
    auto& object = source.CreateGameObject("Water");
    auto& water = object.AddComponent<scene::WaterComponent>();
    water.extentX = water.extentZ = 2.0f;
    water.buoyancyDepth = 4.0f;
    water.waveAttenuationDepth = 2.0f;
    water.waveCount = 0;
    return water;
}
} /// @note namespace

class WaterDepthFieldTest : public testkit::EngineFixture {};

TEST_F(WaterDepthFieldTest, HorizonFollowsOffshoreTerrainAcrossMirroredWaterAxes)
{
    scene::Scene source;
    auto& terrain = AddTerrain(source, -1.0f);
    terrain.transform.position = {96.0f, 0.0f, 36.0f};
    auto& land = AddTerrain(source, 6.0f);
    land.transform.position = {108.0f, 0.0f, 36.0f};
    scene::FlushWorldTransforms(source);
    auto water = MakeWater();
    scene::Transform pose;
    pose.worldRotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(35.0f));
    pose.worldScale = {-2.0f, 1.0f, 0.5f};
    scene::WaterTerrainInfoInputs inputs;
    scene::WaterTerrainInfoCache cache;
    ASSERT_TRUE(cache.TryCollect(source, inputs));
    scene::WaterTerrainBakeSettings settings;
    settings.width = settings.height = 64;
    scene::WaterTerrainInfoAtlas finiteAtlas, horizonAtlas;
    ASSERT_TRUE(scene::TryBakeWaterTerrainInfo(inputs, water, pose, settings, finiteAtlas));
    water.farOceanEnabled = true;
    ASSERT_TRUE(scene::TryBakeWaterTerrainInfo(inputs, water, pose, settings, horizonAtlas));
    EXPECT_NE(finiteAtlas.signature, horizonAtlas.signature);
    EXPECT_GT(horizonAtlas.domain.z, 8.0f);
    EXPECT_GT(horizonAtlas.domain.w, 8.0f);
    scene::WaterDepthField field;
    ASSERT_TRUE(scene::TryBuildWaterDepthField(horizonAtlas, water, pose, field));
    scene::WaterDepthSample sample;
    ASSERT_TRUE(scene::TrySampleWaterDepthField(field, {100.0f, 40.0f}, sample));
    EXPECT_NEAR(sample.factor, 0.5f, 0.001f);
    ASSERT_TRUE(scene::TrySampleWaterDepthField(field, {120.0f, 40.0f}, sample));
    EXPECT_FLOAT_EQ(sample.factor, 1.0f);

    water.waveAttenuationDepth = 0.0f;
    ASSERT_TRUE(scene::TryBuildWaterDepthField(horizonAtlas, water, pose, field));
    water.depthField = std::make_shared<const scene::WaterDepthField>(std::move(field));
    scene::WaterSurfaceHeightSample height;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {100.0f, -0.5f, 40.0f}, 0.0f, height));
    EXPECT_TRUE(height.underwater);
    EXPECT_NEAR(height.waterDepthLimit, 1.0f, 0.001f);
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {100.0f, -1.5f, 40.0f}, 0.0f, height));
    EXPECT_FALSE(height.underwater);
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {112.0f, -0.5f, 40.0f}, 0.0f, height));
    EXPECT_FALSE(height.underwater);
    EXPECT_FLOAT_EQ(height.waterDepthLimit, 0.0f);
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {120.0f, -1.5f, 40.0f}, 0.0f, height));
    EXPECT_TRUE(height.underwater);
}

TEST_F(WaterDepthFieldTest, HorizonCrestWetsALowBankAndTheTroughUncoversIt)
{
    auto water = MakeWater();
    water.farOceanEnabled = true;
    water.waveAttenuationDepth = 6.0f;
    water.waveCount = 1;
    water.waveGrouping = water.waveSpread = 0.0f;
    water.waves[0] = {{0.0f, 1.0f}, 0.8f, 8.0f, 0.5f, math::PI * 0.5f};
    scene::Transform pose;
    auto atlas = MakeAtlas({-0.2f, -2.0f, -0.2f, -2.0f}, 3u);
    atlas.domain = {0.0f, 0.0f, 4.0f, 4.0f};
    scene::WaterDepthField field;
    ASSERT_TRUE(scene::TryBuildWaterDepthField(atlas, water, pose, field));
    water.depthField = std::make_shared<const scene::WaterDepthField>(std::move(field));

    const float troughTime = math::PI / std::sqrt(9.8f * math::TWO_PI / 8.0f);
    const math::Vector3 lowBank{-1.0f, 0.25f, 0.0f};
    scene::WaterSurfaceHeightSample sample;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, lowBank, 0.0f, sample));
    EXPECT_TRUE(sample.underwater);
    EXPECT_GT(sample.waterDepthLimit, 0.25f);
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, lowBank, troughTime, sample));
    EXPECT_FALSE(sample.underwater);
    EXPECT_FLOAT_EQ(sample.waterDepthLimit, 0.0f);
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {1.0f, 2.01f, 0.0f}, 0.0f, sample));
    EXPECT_FLOAT_EQ(sample.waterDepthLimit, 0.0f);
    EXPECT_NEAR(sample.displacement, 0.0f, 0.0001f);
}

TEST_F(WaterDepthFieldTest, HandlesNoBottomDryAndBothClippedDepthSigns)
{
    auto water = MakeWater();
    scene::Transform pose;
    scene::WaterDepthSample sample;

    auto noBottom = MakeAtlas({1.0f, 1.0f, 1.0f, 1.0f}, 0u);
    scene::WaterDepthField field;
    ASSERT_TRUE(scene::TryBuildWaterDepthField(noBottom, water, pose, field));
    ASSERT_TRUE(scene::TrySampleWaterDepthField(field, {}, sample));
    EXPECT_FLOAT_EQ(sample.factor, 1.0f);

    auto dry = MakeAtlas({0.0f, 0.0f, 0.0f, 0.0f}, 3u);
    ASSERT_TRUE(scene::TryBuildWaterDepthField(dry, water, pose, field));
    ASSERT_TRUE(scene::TrySampleWaterDepthField(field, {}, sample));
    EXPECT_FLOAT_EQ(sample.factor, 0.0f);

    auto clippedDeep = MakeAtlas({4.0f, 4.0f, 4.0f, 4.0f}, 5u);
    ASSERT_TRUE(scene::TryBuildWaterDepthField(clippedDeep, water, pose, field));
    ASSERT_TRUE(scene::TrySampleWaterDepthField(field, {}, sample));
    EXPECT_FLOAT_EQ(sample.factor, 1.0f);

    auto clippedAboveWater = MakeAtlas({-4.0f, -4.0f, -4.0f, -4.0f}, 5u);
    ASSERT_TRUE(scene::TryBuildWaterDepthField(clippedAboveWater, water, pose, field));
    ASSERT_TRUE(scene::TrySampleWaterDepthField(field, {}, sample));
    EXPECT_FLOAT_EQ(sample.factor, 0.0f);
}

TEST_F(WaterDepthFieldTest, SmoothsPerTexelBeforeBilinearFilteringAndReportsAnalyticGradient)
{
    auto water = MakeWater();
    scene::Transform pose;
    auto atlas = MakeAtlas({0.0f, 1.0f, 2.0f, 3.0f}, 1u);
    scene::WaterDepthField field;
    ASSERT_TRUE(scene::TryBuildWaterDepthField(atlas, water, pose, field));

    scene::WaterDepthSample sample;
    ASSERT_TRUE(scene::TrySampleWaterDepthField(field, {}, sample));
    EXPECT_NEAR(sample.factor, 0.625f, 0.0001f);
    scene::WaterDepthSample outside;
    ASSERT_TRUE(scene::TrySampleWaterDepthField(field, {4.0f, 0.0f}, outside));
    EXPECT_FLOAT_EQ(outside.factor, 1.0f);

    constexpr float step = 0.01f;
    scene::WaterDepthSample left, right;
    ASSERT_TRUE(scene::TrySampleWaterDepthField(field, {-step, 0.0f}, left));
    ASSERT_TRUE(scene::TrySampleWaterDepthField(field, {step, 0.0f}, right));
    const float finiteDifference = (right.factor - left.factor) / (2.0f * step);
    EXPECT_NEAR(sample.gradient.x, finiteDifference, 0.001f);

    scene::WaterDepthSample unchanged{0.25f, {2.0f, 3.0f}};
    EXPECT_FALSE(scene::TrySampleWaterDepthField(field,
        {std::numeric_limits<float>::quiet_NaN(), 0.0f}, unchanged));
    EXPECT_FLOAT_EQ(unchanged.factor, 0.25f);
    EXPECT_FLOAT_EQ(unchanged.gradient.x, 2.0f);
    EXPECT_FLOAT_EQ(unchanged.gradient.y, 3.0f);
}

TEST_F(WaterDepthFieldTest, FadesOutsideContinuouslyAndKeepsTheEdgeInverseInside)
{
    auto water = MakeWater();
    water.extentX = water.extentZ = 100.0f;
    water.buoyancyDepth = 10.0f;
    water.waveAttenuationDepth = 1.0f;
    water.waveCount = 1;
    water.waveSpread = 0.0f;
    water.waveGrouping = 0.0f;
    water.waves[0] = {{1.0f, 0.0f}, 0.3f, 62.83185f, 0.9f, 4.7f};
    scene::Transform pose;
    const auto atlas = MakeSolidAtlas(101, 101, 0.71286f, 1u, 10.0f);
    scene::WaterDepthField field;
    ASSERT_TRUE(scene::TryBuildWaterDepthField(atlas, water, pose, field));
    EXPECT_FLOAT_EQ(field.outsideFadeWidth, 1.0f);
    EXPECT_NEAR(field.maximumAttenuationGradient, 0.3f, 0.001f);
    water.depthField = std::make_shared<const scene::WaterDepthField>(std::move(field));

    scene::WaterDepthSample boundary, outside, farOutside;
    ASSERT_TRUE(scene::TrySampleWaterDepthField(*water.depthField, {50.0f, 0.0f}, boundary));
    ASSERT_TRUE(scene::TrySampleWaterDepthField(*water.depthField, {50.1f, 0.0f}, outside));
    ASSERT_TRUE(scene::TrySampleWaterDepthField(*water.depthField, {51.1f, 0.0f}, farOutside));
    EXPECT_NEAR(boundary.factor, 0.8f, 0.001f);
    EXPECT_NEAR(boundary.gradient.x, 0.0f, 0.0001f);
    EXPECT_GT(outside.factor, boundary.factor);
    EXPECT_FLOAT_EQ(farOutside.factor, 1.0f);
    EXPECT_FLOAT_EQ(farOutside.gradient.x, 0.0f);
    EXPECT_FLOAT_EQ(farOutside.gradient.y, 0.0f);

    constexpr float step = 0.01f;
    scene::WaterDepthSample left, right;
    ASSERT_TRUE(scene::TrySampleWaterDepthField(*water.depthField, {50.1f - step, 0.0f}, left));
    ASSERT_TRUE(scene::TrySampleWaterDepthField(*water.depthField, {50.1f + step, 0.0f}, right));
    EXPECT_NEAR(outside.gradient.x, (right.factor - left.factor) / (2.0f * step), 0.002f);

    const math::Vector2 base{49.999f, 0.0f};
    const math::Vector2 displacement = water.HorizontalDisplacementAt(base, 0.0f);
    const math::Vector2 displayed{base.x + displacement.x, base.y + displacement.y};
    EXPECT_NEAR(displayed.x, 49.7911233f, 0.001f);
    EXPECT_LT(displayed.x, 50.0f);
    const math::Vector2 recovered = water.SolveUndisplacedXZ(displayed.x, displayed.y, 0.0f);
    EXPECT_NEAR(recovered.x, base.x, 0.001f);
    EXPECT_NEAR(recovered.y, base.y, 0.001f);
    EXPECT_TRUE(scene::WaterSurfaceContainsXZ(water, pose, displayed.x, displayed.y, 0.0f));

    auto deep = water;
    deep.waveAttenuationDepth = 1.0f;
    auto deepAtlas = MakeSolidAtlas(101, 101, 10.0f, 1u, 10.0f);
    scene::WaterDepthField deepField;
    ASSERT_TRUE(scene::TryBuildWaterDepthField(deepAtlas, deep, pose, deepField));
    EXPECT_FLOAT_EQ(deepField.maximumAttenuationGradient, 0.0f);
    deep.depthField = std::make_shared<const scene::WaterDepthField>(std::move(deepField));
    deep.waves[0] = {{1.0f, 0.0f}, 4.0f, 2.0f, 1.0f, 0.0f};
    EXPECT_FLOAT_EQ(deep.GetHorizontalWaveScale(), 1.0f);
}

TEST_F(WaterDepthFieldTest, MapsGradientThroughYawAndNegativeNonUniformScale)
{
    auto water = MakeWater();
    water.extentX = 2.0f;
    water.extentZ = 4.0f;
    scene::Transform pose;
    pose.worldPosition = {4.0f, 2.0f, -3.0f};
    pose.worldRotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(90.0f));
    pose.worldScale = {-2.0f, 1.0f, 0.5f};
    auto atlas = MakeAtlas({0.0f, 2.0f, 0.0f, 2.0f}, 1u);
    scene::WaterDepthField field;
    ASSERT_TRUE(scene::TryBuildWaterDepthField(atlas, water, pose, field));

    scene::WaterDepthSample sample;
    ASSERT_TRUE(scene::TrySampleWaterDepthField(field, {pose.worldPosition.x, pose.worldPosition.z}, sample));
    EXPECT_NEAR(sample.factor, 0.5f, 0.0001f);
    const math::Vector3 localXAxis = pose.worldRotation * math::Vector3::RIGHT;
    const float directionalGradient = sample.gradient.x * localXAxis.x + sample.gradient.y * localXAxis.z;
    EXPECT_NEAR(directionalGradient, -0.5f, 0.001f);
}

TEST_F(WaterDepthFieldTest, AttenuatesWaveHeightAndHorizontalPushWithTheSameInverseMap)
{
    auto water = MakeWater();
    water.extentX = 8.0f;
    water.waveCount = 1;
    water.waveSpread = 0.0f;
    water.waveGrouping = 0.0f;
    water.waves[0] = {{1.0f, 0.0f}, 0.2f, 8.0f, 0.5f};
    auto atlas = MakeAtlas({1.0f, 1.0f, 1.0f, 1.0f}, 1u);
    scene::Transform pose;
    scene::WaterDepthField field;
    ASSERT_TRUE(scene::TryBuildWaterDepthField(atlas, water, pose, field));
    water.depthField = std::make_shared<const scene::WaterDepthField>(std::move(field));

    EXPECT_FLOAT_EQ(water.GetHorizontalWaveScale(), 1.0f);
    const math::Vector2 push = water.HorizontalDisplacementAt({}, 0.0f);
    EXPECT_NEAR(push.x, 0.05f, 0.0001f);
    EXPECT_FLOAT_EQ(push.y, 0.0f);
    EXPECT_NEAR(water.GetSurfaceHeightAt(2.0f, 0.0f, 0.0f), 0.1f, 0.0001f);

    const math::Vector2 displayed = push;
    const math::Vector2 recovered = water.SolveUndisplacedXZ(displayed.x, displayed.y, 0.0f);
    EXPECT_NEAR(recovered.x, 0.0f, 0.0001f);
    EXPECT_NEAR(recovered.y, 0.0f, 0.0001f);

    scene::WaterComponent legacy = water;
    legacy.waveAttenuationDepth = 0.0f;
    EXPECT_FLOAT_EQ(legacy.GetHorizontalWaveScale(), 1.0f);
    EXPECT_NEAR(legacy.GetSurfaceHeightAt(2.0f, 0.0f, 0.0f), 0.2f, 0.0001f);

    auto gradientAtlas = MakeAtlas({0.0f, 2.0f, 0.0f, 2.0f}, 1u);
    scene::WaterDepthField gradientField;
    ASSERT_TRUE(scene::TryBuildWaterDepthField(gradientAtlas, water, pose, gradientField));
    scene::WaterComponent scaled = water;
    scaled.depthField = std::make_shared<const scene::WaterDepthField>(std::move(gradientField));
    scaled.waves[0] = {{1.0f, 0.0f}, 4.0f, 2.0f, 1.0f};
    EXPECT_LT(scaled.GetHorizontalWaveScale(), 1.0f);
    EXPECT_GE(scaled.GetHorizontalWaveScale(), 0.0f);
}

TEST_F(WaterDepthFieldTest, PreparationIsRenderingIndependentAndKeysSnapshotsByTerrainLifetime)
{
    scene::Scene source;
    auto& terrainObject = AddTerrain(source, -1.0f);
    const auto terrainId = terrainObject.GetID();
    auto& water = AddWater(source);
    water.waveCount = 1;
    water.waveSpread = 0.0f;
    water.waveGrouping = 0.0f;
    water.waves[0] = {{1.0f, 0.0f}, 0.5f, 4.0f, 0.0f};
    scene::FlushWorldTransforms(source);

    scene::PrepareWaterDepthFields(source);
    ASSERT_NE(water.depthField, nullptr);
    const auto firstField = water.depthField;
    const auto firstSignature = firstField->signature;
    scene::WaterDepthSample firstSample;
    ASSERT_TRUE(scene::TrySampleWaterDepthField(*firstField, {}, firstSample));
    EXPECT_NEAR(firstSample.factor, 0.5f, 0.001f);

    scene::PrepareWaterSimulationSurfaces(source);
    ASSERT_EQ(water.simulationDepthField, firstField);
    EXPECT_EQ(water.EffectiveDepthField(scene::WaterSurfaceReadView::SIMULATION), firstField.get());
    EXPECT_FLOAT_EQ(water.EffectiveWaveAttenuationDepth(scene::WaterSurfaceReadView::SIMULATION), 2.0f);
    const float capturedSimulationHeight = water.GetSurfaceHeightAt(
        0.5f, 0.0f, 0.0f, scene::WaterSurfaceReadView::SIMULATION);
    EXPECT_GT(capturedSimulationHeight, 0.0f);
    scene::WaterComponent copiedWater = water;
    copiedWater.ResetSurfaceRuntimeState();
    EXPECT_EQ(copiedWater.simulationDepthField, nullptr);
    EXPECT_FALSE(copiedWater.simulationSurfacePublished);
    EXPECT_FALSE(copiedWater.simulationReadEnabled);
    const scene::WaterComponent physicsSnapshot = water;
    scene::PrepareWaterDepthFields(source);
    EXPECT_EQ(water.depthField, firstField);
    scene::PrepareWaterDepthFields(source, scene::WaterSurfaceReadView::SIMULATION);
    EXPECT_EQ(water.simulationDepthField, firstField);

    auto* terrain = source.GetComponent<scene::TerrainComponent>(terrainId);
    ASSERT_NE(terrain, nullptr);
    terrain->InitFlat(0.5f);
    scene::PrepareWaterDepthFields(source);
    ASSERT_NE(water.depthField, firstField);
    EXPECT_NE(water.depthField->signature, firstSignature);
    EXPECT_EQ(water.simulationDepthField, firstField);
    EXPECT_EQ(water.EffectiveDepthField(scene::WaterSurfaceReadView::SIMULATION), firstField.get());
    EXPECT_NEAR(water.GetSurfaceHeightAt(0.5f, 0.0f, 0.0f,
        scene::WaterSurfaceReadView::SIMULATION), capturedSimulationHeight, 0.0001f);
    EXPECT_NEAR(water.GetSurfaceHeightAt(0.5f, 0.0f, 0.0f,
        scene::WaterSurfaceReadView::LIVE_QUERY), 0.0f, 0.0001f);
    scene::WaterDepthSample snapshotSample, currentSample;
    ASSERT_TRUE(scene::TrySampleWaterDepthField(*physicsSnapshot.simulationDepthField, {}, snapshotSample));
    ASSERT_TRUE(scene::TrySampleWaterDepthField(*water.depthField, {}, currentSample));
    EXPECT_NEAR(snapshotSample.factor, 0.5f, 0.001f);
    EXPECT_FLOAT_EQ(currentSample.factor, 0.0f);

    const uint64_t oldLifetime = source.GetComponentLifetime<scene::TerrainComponent>(terrainId);
    const uint64_t sameRevision = terrain->surfaceRevision;
    source.RemoveComponent<scene::TerrainComponent>(terrainId);
    auto& replacement = terrainObject.AddComponent<scene::TerrainComponent>();
    replacement.columns = replacement.rows = 9;
    replacement.cellSize = replacement.maxHeight = 1.0f;
    replacement.InitFlat(0.0f);
    replacement.InitFlat(0.5f);
    terrainObject.transform.position = {-4.0f, 0.0f, -4.0f};
    scene::FlushWorldTransforms(source);
    EXPECT_EQ(replacement.surfaceRevision, sameRevision);
    EXPECT_NE(source.GetComponentLifetime<scene::TerrainComponent>(terrainId), oldLifetime);

    const auto beforeReplacement = water.depthField;
    scene::PrepareWaterDepthFields(source);
    ASSERT_NE(water.depthField, beforeReplacement);
    const auto afterReplacement = water.depthField;
    scene::PrepareWaterDepthFields(source);
    EXPECT_EQ(water.depthField, afterReplacement);
    EXPECT_EQ(water.simulationDepthField, firstField);
}

TEST_F(WaterDepthFieldTest, PausedWaterSystemKeepsPublishedSimulationDepthWhileRefreshingLiveField)
{
    scene::Scene source;
    auto& terrainObject = AddTerrain(source, -1.0f);
    auto& water = AddWater(source);
    scene::FlushWorldTransforms(source);

    physics::World physicsWorld;
    scene::WaterSystem waterSystem;
    SystemContext pausedContext{source, physicsWorld, nullptr, nullptr, 0.0f, 0.0f, false, true};
    waterSystem.Update(pausedContext);
    ASSERT_TRUE(water.simulationSurfacePublished);
    ASSERT_NE(water.simulationDepthField, nullptr);
    const auto publishedField = water.simulationDepthField;

    auto* terrain = source.GetComponent<scene::TerrainComponent>(terrainObject.GetID());
    ASSERT_NE(terrain, nullptr);
    terrain->InitFlat(0.5f);
    waterSystem.Update(pausedContext);

    ASSERT_NE(water.depthField, nullptr);
    EXPECT_NE(water.depthField, publishedField);
    EXPECT_EQ(water.simulationDepthField, publishedField);
    EXPECT_EQ(water.EffectiveDepthField(scene::WaterSurfaceReadView::SIMULATION), publishedField.get());
    EXPECT_EQ(water.EffectiveDepthField(scene::WaterSurfaceReadView::LIVE_QUERY), water.depthField.get());
}

TEST_F(WaterDepthFieldTest, ZoneDepthFieldUsesSharedBoundsAndReactivatesMemberOnDisable)
{
    scene::Scene source;
    AddTerrain(source, -1.0f);

    auto& ownerObject = source.CreateGameObject("Zone Owner");
    auto& ownerWater = ownerObject.AddComponent<scene::WaterComponent>();
    ownerWater.extentX = ownerWater.extentZ = 4.0f;
    ownerWater.waveAttenuationDepth = 2.0f;

    auto& memberObject = source.CreateGameObject("Zone Member");
    memberObject.transform.position.x = 7.0f;
    auto& memberWater = memberObject.AddComponent<scene::WaterComponent>();
    memberWater.extentX = memberWater.extentZ = 6.0f;
    memberWater.waveAttenuationDepth = 2.0f;

    auto& zone = ownerObject.AddComponent<scene::WaterZoneComponent>();
    zone.members.push_back({scene::EntityRef{memberObject.GetID()}, 0});
    scene::FlushWorldTransforms(source);

    scene::PrepareWaterSceneSurfaces(source);
    ASSERT_NE(ownerWater.zoneSnapshot, nullptr) << zone.diagnostic;
    ASSERT_TRUE(memberWater.zoneOwner.IsValid());
    const auto effectiveExtent = scene::WaterSurfaceLocalExtent(ownerWater);
    EXPECT_GT(effectiveExtent.x, ownerWater.extentX);

    scene::PrepareWaterDepthFields(source);
    ASSERT_NE(ownerWater.depthField, nullptr);
    EXPECT_NEAR(ownerWater.depthField->mapping.extentX, effectiveExtent.x, 0.0001f);
    EXPECT_NEAR(ownerWater.depthField->mapping.extentZ, effectiveExtent.y, 0.0001f);
    EXPECT_FALSE(memberWater.depthField);
    const auto sharedField = ownerWater.depthField;

    memberObject.transform.position.x = 9.0f;
    scene::FlushWorldTransforms(source);
    scene::PrepareWaterSceneSurfaces(source);
    scene::PrepareWaterDepthFields(source);
    ASSERT_NE(ownerWater.depthField, nullptr);
    EXPECT_NE(ownerWater.depthField, sharedField);
    EXPECT_NE(ownerWater.depthField->signature, sharedField->signature);

    zone.enabled = false;
    scene::PrepareWaterSceneSurfaces(source);
    scene::PrepareWaterDepthFields(source);
    EXPECT_FALSE(memberWater.zoneOwner.IsValid());
    EXPECT_NE(memberWater.depthField, nullptr);
    ASSERT_NE(ownerWater.depthField, nullptr);
    EXPECT_NEAR(ownerWater.depthField->mapping.extentX, ownerWater.extentX, 0.0001f);
}
} /// @note namespace fbzz::tests

