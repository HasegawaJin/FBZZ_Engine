/// @file    WaterTerrainInfoTests.cpp
/// @brief   複数地形・穴・world TRS の水深と岸泡 atlas の更新・符号付き量子化を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/WaterTerrainInfo.hpp>
#include <Math/MathUtils.hpp>
#include <cmath>
#include <limits>

namespace fbzz::tests {
namespace {
scene::GameObject& AddTerrain(scene::Scene& source, float height)
{
    auto& object = source.CreateGameObject("Terrain");
    auto& terrain = object.AddComponent<scene::TerrainComponent>();
    terrain.columns = terrain.rows = 3;
    terrain.cellSize = terrain.maxHeight = 1.0f;
    terrain.InitFlat(height);
    return object;
}
scene::WaterComponent Water()
{
    scene::WaterComponent water;
    water.enableGerstnerWaves = false;
    water.extentX = water.extentZ = 2.0f;
    water.buoyancyDepth = 2.0f;
    return water;
}
void ExpectSampleUnchanged(const scene::WaterTerrainSample& sample)
{
    EXPECT_TRUE(sample.hasBottom);
    EXPECT_FLOAT_EQ(sample.bottomY, 7.0f);
    EXPECT_DOUBLE_EQ(sample.signedDepth, 9.0);
    EXPECT_FLOAT_EQ(sample.depth, 9.0f);
    EXPECT_TRUE(sample.dry);
    EXPECT_FLOAT_EQ(sample.shoreFoam, 0.8f);
}
} /// @note namespace

class WaterTerrainInfoTest : public testkit::EngineFixture {};

TEST_F(WaterTerrainInfoTest, SelectsTheHighestTerrainEvenWhenItIsAboveTheWater)
{
    scene::Scene source;
    AddTerrain(source, -2.0f);
    AddTerrain(source, 0.75f);
    scene::WaterTerrainInfoCache cache;
    scene::WaterTerrainInfoInputs inputs;
    ASSERT_TRUE(cache.TryCollect(source, inputs));
    scene::WaterTerrainSample sample;
    ASSERT_TRUE(scene::TrySampleWaterTerrainInfo(inputs, {0.25f, 0.25f}, 1.0f, 8.0f, 0.3f, 0.5f, sample));
    EXPECT_TRUE(sample.hasBottom);
    EXPECT_FLOAT_EQ(sample.bottomY, 0.75f);
    EXPECT_DOUBLE_EQ(sample.signedDepth, 0.25);
    EXPECT_FALSE(sample.dry);
    EXPECT_GT(sample.shoreFoam, 0.0f);
    AddTerrain(source, 2.0f);
    ASSERT_TRUE(cache.TryCollect(source, inputs));
    ASSERT_TRUE(scene::TrySampleWaterTerrainInfo(inputs, {0.25f, 0.25f}, 1.0f, 8.0f, 0.3f, 0.5f, sample));
    EXPECT_FLOAT_EQ(sample.bottomY, 2.0f);
    EXPECT_DOUBLE_EQ(sample.signedDepth, -1.0);
    EXPECT_TRUE(sample.dry);
    EXPECT_FLOAT_EQ(sample.depth, 0.0f);
    EXPECT_FLOAT_EQ(sample.shoreFoam, 0.0f);
}

TEST_F(WaterTerrainInfoTest, HolesRevealTheLowerTerrainAndThenExplicitlyReportNoBottom)
{
    scene::Scene source;
    const auto lowerId = AddTerrain(source, -2.0f).GetID();
    const auto upperId = AddTerrain(source, 0.75f).GetID();
    auto* upper = source.GetComponent<scene::TerrainComponent>(upperId);
    ASSERT_TRUE(upper->SetHoleCell(0, 0, true));
    upper->RequestHoleRebuild();
    scene::WaterTerrainInfoCache cache;
    scene::WaterTerrainInfoInputs inputs;
    ASSERT_TRUE(cache.TryCollect(source, inputs));
    scene::WaterTerrainSample sample;
    ASSERT_TRUE(scene::TrySampleWaterTerrainInfo(inputs, {0.25f, 0.25f}, 1.0f, 8.0f, 0.3f, 0.5f, sample));
    EXPECT_TRUE(sample.hasBottom);
    EXPECT_FLOAT_EQ(sample.bottomY, -2.0f);
    auto* lower = source.GetComponent<scene::TerrainComponent>(lowerId);
    ASSERT_TRUE(lower->SetHoleCell(0, 0, true));
    lower->RequestHoleRebuild();
    ASSERT_TRUE(cache.TryCollect(source, inputs));
    ASSERT_TRUE(scene::TrySampleWaterTerrainInfo(inputs, {0.25f, 0.25f}, 1.0f, 8.0f, 0.3f, 0.5f, sample));
    EXPECT_FALSE(sample.hasBottom);
    EXPECT_FALSE(sample.dry);
    EXPECT_FLOAT_EQ(sample.depth, 8.0f);
    EXPECT_FLOAT_EQ(sample.shoreFoam, 0.0f);
}

TEST_F(WaterTerrainInfoTest, RespectsParentYawSignedScaleAndWaterUvWorldMapping)
{
    scene::Scene source;
    const auto terrainId = AddTerrain(source, 0.25f).GetID();
    auto& parent = source.CreateGameObject("Parent");
    parent.transform.position = {10.0f, 4.0f, 20.0f};
    parent.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(45.0f));
    parent.transform.scale = {2.0f, 3.0f, 0.5f};
    auto* object = source.GetGameObject(terrainId);
    object->transform.position = {1.0f, 0.0f, 1.0f};
    object->transform.scale.x = -1.0f;
    object->SetParent(parent);
    scene::FlushWorldTransforms(source);
    const auto point = object->transform.GetWorldMatrix() * math::Vector4{0.5f, 0.25f, 0.5f, 1.0f};
    scene::WaterTerrainInfoCache cache;
    scene::WaterTerrainInfoInputs inputs;
    ASSERT_TRUE(cache.TryCollect(source, inputs));
    scene::WaterTerrainSample sample;
    ASSERT_TRUE(scene::TrySampleWaterTerrainInfo(inputs, {point.x, point.z}, 5.0f, 2.0f, 0.3f, 0.5f, sample));
    EXPECT_TRUE(sample.hasBottom);
    EXPECT_NEAR(sample.bottomY, 4.75f, testkit::kTolerance);
    auto water = Water();
    water.extentX = 0.5f; water.extentZ = 0.25f;
    scene::Transform pose;
    pose.worldPosition = {point.x, 5.0f, point.z};
    pose.worldRotation = parent.transform.worldRotation;
    pose.worldScale.x = -1.0f;
    scene::WaterTerrainBakeSettings settings;
    settings.width = settings.height = 2;
    scene::WaterTerrainInfoAtlas atlas;
    ASSERT_TRUE(scene::TryBakeWaterTerrainInfo(inputs, water, pose, settings, atlas));
    for (size_t pixel = 0; pixel < 4; ++pixel) {
        EXPECT_EQ(atlas.rgba[pixel * 4 + 3], 1u);
        float depth = 0.0f;
        ASSERT_TRUE(scene::TryDecodeWaterTerrainDepth(atlas.rgba[pixel * 4 + 1], atlas.rgba[pixel * 4 + 2], atlas.depthRange, depth));
        EXPECT_NEAR(depth, 0.25f, atlas.depthRange / 65535.0f + 0.00001f);
    }
}

TEST_F(WaterTerrainInfoTest, UsesTheExistingInclinedTerrainVerticalIntersection)
{
    scene::Scene source;
    auto& object = AddTerrain(source, 0.0f);
    object.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, math::ToRad(30.0f));
    object.transform.scale.y = 2.0f;
    scene::FlushWorldTransforms(source);
    scene::WaterTerrainInfoCache cache;
    scene::WaterTerrainInfoInputs inputs;
    ASSERT_TRUE(cache.TryCollect(source, inputs));
    scene::WaterTerrainSample sample;
    ASSERT_TRUE(scene::TrySampleWaterTerrainInfo(inputs, {0.5f, 0.5f}, 1.0f, 2.0f, 0.3f, 0.5f, sample));
    EXPECT_TRUE(sample.hasBottom);
    EXPECT_NEAR(sample.bottomY, 0.5f * std::tan(math::ToRad(30.0f)), testkit::kLooseTolerance);
}

TEST_F(WaterTerrainInfoTest, UpdatesSignaturesForConsumedDirtyContentGeometryPoseWaterAndFoam)
{
    scene::Scene source;
    const auto id = AddTerrain(source, 0.0f).GetID();
    scene::WaterTerrainInfoCache cache;
    scene::WaterTerrainInfoInputs first, changed;
    ASSERT_TRUE(cache.TryCollect(source, first));
    auto* terrain = source.GetComponent<scene::TerrainComponent>(id);
    ASSERT_TRUE(terrain->SetHeightAtGrid(0, 0, 0.5f));
    terrain->heightDirty = false;
    ASSERT_TRUE(cache.TryCollect(source, changed));
    EXPECT_NE(first.signature, changed.signature);
    first = changed;
    terrain->maxHeight = 2.0f;
    ASSERT_TRUE(cache.TryCollect(source, changed));
    EXPECT_NE(first.signature, changed.signature);
    first = changed;
    source.GetGameObject(id)->transform.position.x = 1.0f;
    scene::FlushWorldTransforms(source);
    ASSERT_TRUE(cache.TryCollect(source, changed));
    EXPECT_NE(first.signature, changed.signature);
    auto water = Water();
    scene::Transform pose;
    scene::WaterTerrainBakeSettings settings;
    uint64_t original = 0, replacement = 0;
    ASSERT_TRUE(scene::TryComputeWaterTerrainInfoSignature(changed, water, pose, settings, original));
    settings.foamThreshold = 0.6f;
    ASSERT_TRUE(scene::TryComputeWaterTerrainInfoSignature(changed, water, pose, settings, replacement));
    EXPECT_NE(original, replacement);
    original = replacement;
    water.buoyancyDepth = 3.0f;
    ASSERT_TRUE(scene::TryComputeWaterTerrainInfoSignature(changed, water, pose, settings, replacement));
    EXPECT_NE(original, replacement);
    original = replacement;
    pose.worldPosition.z = 5.0f;
    ASSERT_TRUE(scene::TryComputeWaterTerrainInfoSignature(changed, water, pose, settings, replacement));
    EXPECT_NE(original, replacement);
    original = replacement;
    water.extentX = 3.0f;
    ASSERT_TRUE(scene::TryComputeWaterTerrainInfoSignature(changed, water, pose, settings, replacement));
    EXPECT_NE(original, replacement);
}

TEST_F(WaterTerrainInfoTest, PacksSignedDepthAtTexelCentersWithinTheSixteenBitQuantizationError)
{
    scene::Scene source;
    auto& terrain = *AddTerrain(source, 0.0f).GetComponent<scene::TerrainComponent>();
    terrain.columns = terrain.rows = 2;
    terrain.cellSize = 2.0f;
    terrain.heightData = {-2.0f, 2.0f, -2.0f, 2.0f};
    terrain.RequestHeightRebuild();
    scene::WaterTerrainInfoCache cache;
    scene::WaterTerrainInfoInputs inputs;
    ASSERT_TRUE(cache.TryCollect(source, inputs));
    const auto water = Water();
    scene::Transform pose;
    pose.worldPosition = {1.0f, 0.0f, 1.0f};
    scene::WaterTerrainBakeSettings settings;
    settings.width = 5; settings.height = 1;
    settings.foamThreshold = 1.0f;
    scene::WaterTerrainInfoAtlas atlas;
    ASSERT_TRUE(scene::TryBakeWaterTerrainInfo(inputs, water, pose, settings, atlas));
    ASSERT_EQ(atlas.rgba.size(), 20u);
    for (uint32_t x = 0; x < 5; ++x) {
        const float worldX = (static_cast<float>(x) + 0.5f) / 5.0f * 2.0f;
        const float expected = 2.0f - 2.0f * worldX;
        float decoded = 0.0f;
        ASSERT_TRUE(scene::TryDecodeWaterTerrainDepth(atlas.rgba[x * 4 + 1], atlas.rgba[x * 4 + 2], atlas.depthRange, decoded));
        EXPECT_NEAR(decoded, expected, atlas.depthRange / 65535.0f + 0.000001f);
        EXPECT_EQ(atlas.rgba[x * 4 + 3], expected <= 0.0f ? 3u : 1u);
        scene::WaterTerrainSample sample;
        ASSERT_TRUE(scene::TrySampleWaterTerrainInfo(inputs, {worldX, 1.0f}, 0.0f, 2.0f, settings.foamThreshold, settings.foamFade, sample));
        EXPECT_NEAR(static_cast<float>(atlas.rgba[x * 4]) / 255.0f, sample.shoreFoam, 0.5f / 255.0f + 0.000001f);
    }
}

TEST_F(WaterTerrainInfoTest, NoBottomAndInactiveTerrainProduceZeroFoamAndExplicitFlags)
{
    scene::Scene source;
    AddTerrain(source, 0.0f).SetActive(false);
    scene::WaterTerrainInfoCache cache;
    scene::WaterTerrainInfoInputs inputs;
    ASSERT_TRUE(cache.TryCollect(source, inputs));
    EXPECT_TRUE(inputs.terrains.empty());
    scene::WaterTerrainBakeSettings settings;
    settings.width = settings.height = 1;
    scene::WaterTerrainInfoAtlas atlas;
    ASSERT_TRUE(scene::TryBakeWaterTerrainInfo(inputs, Water(), {}, settings, atlas));
    EXPECT_EQ(atlas.rgba[0], 0u);
    EXPECT_EQ(atlas.rgba[3], 0u);
}

TEST_F(WaterTerrainInfoTest, MarksClippedDepthWithoutInventingFoamOrRejectingAnExactRangeEndpoint)
{
    scene::Scene source;
    const auto id = AddTerrain(source, -2.0f).GetID();
    scene::WaterTerrainInfoCache cache;
    scene::WaterTerrainInfoInputs inputs;
    ASSERT_TRUE(cache.TryCollect(source, inputs));
    auto water = Water();
    water.buoyancyDepth = 0.1f;
    scene::Transform pose;
    pose.worldPosition = {1.0f, 0.0f, 1.0f};
    scene::WaterTerrainBakeSettings settings;
    settings.width = settings.height = 1;
    scene::WaterTerrainInfoAtlas atlas;
    ASSERT_TRUE(scene::TryBakeWaterTerrainInfo(inputs, water, pose, settings, atlas));
    EXPECT_EQ(atlas.rgba[0], 0u);
    EXPECT_EQ(atlas.rgba[1], 255u);
    EXPECT_EQ(atlas.rgba[2], 255u);
    EXPECT_EQ(atlas.rgba[3], 5u);
    source.GetComponent<scene::TerrainComponent>(id)->InitFlat(-water.buoyancyDepth);
    ASSERT_TRUE(cache.TryCollect(source, inputs));
    ASSERT_TRUE(scene::TryBakeWaterTerrainInfo(inputs, water, pose, settings, atlas));
    EXPECT_EQ(atlas.rgba[3], 1u);
}

TEST_F(WaterTerrainInfoTest, RevalidatesWhenATerrainComponentIsReaddedToTheSameEntityAndRevision)
{
    scene::Scene source;
    const auto id = AddTerrain(source, 0.0f).GetID();
    auto* object = source.GetGameObject(id);
    object->transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, math::ToRad(45.0f));
    scene::FlushWorldTransforms(source);
    const auto birth = source.GetComponentLifetime<scene::TerrainComponent>(id);
    const auto revision = source.GetComponent<scene::TerrainComponent>(id)->surfaceRevision;
    scene::WaterTerrainInfoCache cache;
    scene::WaterTerrainInfoInputs first, replaced;
    ASSERT_TRUE(cache.TryCollect(source, first));
    source.RemoveComponent<scene::TerrainComponent>(id);
    auto& terrain = object->AddComponent<scene::TerrainComponent>();
    terrain.columns = terrain.rows = 3;
    terrain.cellSize = terrain.maxHeight = 1.0f;
    terrain.InitFlat(2.0f);
    EXPECT_EQ(terrain.surfaceRevision, revision);
    EXPECT_NE(source.GetComponentLifetime<scene::TerrainComponent>(id), birth);
    ASSERT_TRUE(cache.TryCollect(source, replaced));
    EXPECT_NE(first.signature, replaced.signature);
    const auto point = object->transform.GetWorldMatrix() * math::Vector4{0.5f, 2.0f, 0.5f, 1.0f};
    scene::WaterTerrainSample sample;
    ASSERT_TRUE(scene::TrySampleWaterTerrainInfo(replaced, {point.x, point.z}, 3.0f, 4.0f, 0.3f, 0.5f, sample));
    EXPECT_TRUE(sample.hasBottom);
    EXPECT_NEAR(sample.bottomY, point.y, testkit::kLooseTolerance);
}

TEST_F(WaterTerrainInfoTest, InvalidTerrainContentAndQueryInputsKeepThePreviousOutputs)
{
    scene::Scene source;
    const auto id = AddTerrain(source, 0.0f).GetID();
    scene::WaterTerrainInfoCache cache;
    scene::WaterTerrainInfoInputs inputs;
    ASSERT_TRUE(cache.TryCollect(source, inputs));
    const auto signature = inputs.signature;
    auto* terrain = source.GetComponent<scene::TerrainComponent>(id);
    terrain->heightData[0] = std::numeric_limits<float>::quiet_NaN();
    terrain->RequestHeightRebuild();
    EXPECT_FALSE(cache.TryCollect(source, inputs));
    EXPECT_EQ(inputs.signature, signature);
    EXPECT_EQ(inputs.terrains.size(), 1u);
    scene::WaterTerrainSample sample{true, 7.0f, 9.0, 9.0f, true, 0.8f};
    EXPECT_FALSE(scene::TrySampleWaterTerrainInfo({}, {std::numeric_limits<float>::quiet_NaN(), 0.0f}, 0.0f, 2.0f, 0.3f, 0.5f, sample));
    ExpectSampleUnchanged(sample);
    EXPECT_FALSE(scene::TrySampleWaterTerrainInfo({}, {}, 0.0f, std::numeric_limits<float>::infinity(), 0.3f, 0.5f, sample));
    ExpectSampleUnchanged(sample);
    scene::WaterTerrainInfoAtlas atlas{4, 4, 9.0f, 123, {42}};
    scene::WaterTerrainBakeSettings settings;
    settings.width = 258;
    EXPECT_FALSE(scene::TryBakeWaterTerrainInfo({}, Water(), {}, settings, atlas));
    EXPECT_EQ(atlas.width, 4u);
    EXPECT_FLOAT_EQ(atlas.depthRange, 9.0f);
    EXPECT_EQ(atlas.signature, 123u);
    EXPECT_EQ(atlas.rgba, std::vector<uint8_t>{42});
    float decoded = 7.0f;
    EXPECT_FALSE(scene::TryDecodeWaterTerrainDepth(0, 0, std::numeric_limits<float>::quiet_NaN(), decoded));
    EXPECT_FLOAT_EQ(decoded, 7.0f);
}
TEST_F(WaterTerrainInfoTest, SplineAtlasSharesTheLocalBaselineDepthAndShapeSignature)
{
    auto water = Water();
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.buoyancyDepth = 10.0f;
    water.splinePoints = {{1, {-2.0f, 2.0f, 0.0f}, 2.0f, 4.0f, 1.0f},
                         {2, { 2.0f, 0.0f, 0.0f}, 2.0f, 4.0f, 1.0f}};
    scene::Transform transform;
    scene::WaterSplineSnapshot snapshot;
    ASSERT_TRUE(scene::BuildWaterSplineSnapshot(water.surfaceShape, water.splinePoints, 1, transform, snapshot));
    water.splineSnapshot = std::make_shared<const scene::WaterSplineSnapshot>(std::move(snapshot));
    scene::WaterTerrainBakeSettings settings;
    settings.width = settings.height = 3;
    settings.depthRange = 10.0f;
    scene::WaterTerrainInfoInputs inputs;
    scene::WaterTerrainInfoAtlas virtualBottom;
    ASSERT_TRUE(scene::TryBakeWaterTerrainInfo(inputs, water, transform, settings, virtualBottom));
    constexpr size_t CENTER_PIXEL = (1u * 3u + 1u) * 4u;
    EXPECT_EQ(virtualBottom.rgba[CENTER_PIXEL + 3] & 1u, 1u);
    float depth = 0.0f;
    ASSERT_TRUE(scene::TryDecodeWaterTerrainDepth(virtualBottom.rgba[CENTER_PIXEL + 1],
        virtualBottom.rgba[CENTER_PIXEL + 2], virtualBottom.depthRange, depth));
    EXPECT_NEAR(depth, 4.0f, 0.001f);

    scene::Scene source;
    AddTerrain(source, 0.5f);
    scene::WaterTerrainInfoCache cache;
    ASSERT_TRUE(cache.TryCollect(source, inputs));
    scene::WaterTerrainInfoAtlas groundBottom;
    ASSERT_TRUE(scene::TryBakeWaterTerrainInfo(inputs, water, transform, settings, groundBottom));
    ASSERT_TRUE(scene::TryDecodeWaterTerrainDepth(groundBottom.rgba[CENTER_PIXEL + 1],
        groundBottom.rgba[CENTER_PIXEL + 2], groundBottom.depthRange, depth));
    EXPECT_NEAR(depth, 0.5f, 0.001f);
    water.splinePoints[0].localPosition.y += 1.0f;
    ASSERT_TRUE(scene::BuildWaterSplineSnapshot(water.surfaceShape, water.splinePoints, 1, transform, snapshot));
    water.splineSnapshot = std::make_shared<const scene::WaterSplineSnapshot>(std::move(snapshot));
    scene::WaterTerrainInfoAtlas edited;
    ASSERT_TRUE(scene::TryBakeWaterTerrainInfo(inputs, water, transform, settings, edited));
    EXPECT_NE(edited.signature, groundBottom.signature);
    ASSERT_TRUE(scene::TryDecodeWaterTerrainDepth(edited.rgba[CENTER_PIXEL + 1],
        edited.rgba[CENTER_PIXEL + 2], edited.depthRange, depth));
    EXPECT_NEAR(depth, 1.0f, 0.001f);
}

TEST_F(WaterTerrainInfoTest, CaveSurfaceExpandsBoundsAndInvalidatesExteriorDepth)
{
    scene::Scene source;
    const auto id = AddTerrain(source, 0).GetID();
    auto* terrain = source.GetComponent<scene::TerrainComponent>(id);
    ASSERT_TRUE(terrain->SetHoleCell(0,0,true));
    ASSERT_TRUE(terrain->SetSurfaceGeometry({{0,-6,0},{0,-6,1},{1,-6,0}}, {0,1,2}));
    scene::WaterTerrainInfoCache cache;
    scene::WaterTerrainInfoInputs inputs;
    ASSERT_TRUE(cache.TryCollect(source, inputs));
    ASSERT_EQ(inputs.terrains.size(), 1u);
    EXPECT_FLOAT_EQ(inputs.terrains[0].boundsMin.y, -6);
    const auto signature = inputs.signature;
    scene::WaterTerrainSample sample;
    ASSERT_TRUE(scene::TrySampleWaterTerrainInfo(inputs, {.25f,.25f}, 2, 20, .3f, .5f, sample));
    EXPECT_TRUE(sample.hasBottom); EXPECT_FLOAT_EQ(sample.bottomY, -6); EXPECT_FLOAT_EQ(sample.depth, 8);
    /// @note This atlas remains exterior/topmost data; camera occupancy uses the directional floor query.
    ASSERT_TRUE(terrain->SetSurfaceGeometry({{0,-2,0},{0,-2,1},{1,-2,0}}, {0,1,2}));
    ASSERT_TRUE(cache.TryCollect(source, inputs));
    EXPECT_NE(inputs.signature, signature);
    ASSERT_TRUE(scene::TrySampleWaterTerrainInfo(inputs, {.25f,.25f}, 2, 20, .3f, .5f, sample));
    EXPECT_FLOAT_EQ(sample.bottomY, -2); EXPECT_FLOAT_EQ(sample.depth, 4);
}
} /// @note namespace fbzz::tests
