/// @file    WaterSplineTests.cpp
/// @brief   共有水スプライン形状・高さ・水深・流速と反射往復を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/ComponentReflectionCodec.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Fields/FlowFieldEval.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Physics/World.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <toml++/toml.hpp>
#include <utility>
#include <vector>

namespace fbzz::tests {

class WaterSplineTest : public testkit::Fixture {};

TEST_F(WaterSplineTest, SamplesVariableRiverHeightDepthAndSignedWorldFlowFromSharedTriangles)
{
    scene::WaterComponent water;
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.buoyancyDepth = 5.0f;
    water.current = {100.0f, -200.0f};
    water.splineSamplesPerSegment = 4;
    water.splineMeshCellSize = 100.0f;
    water.waveCount = 0;
    water.splinePoints.resize(2);
    water.splinePoints[0].id = 1;
    water.splinePoints[0].localPosition = {0.0f, 0.0f, 0.0f};
    water.splinePoints[0].width = 4.0f;
    water.splinePoints[0].depth = 2.0f;
    water.splinePoints[0].flowSpeed = 2.0f;
    water.splinePoints[1].id = 2;
    water.splinePoints[1].localPosition = {10.0f, 2.0f, 0.0f};
    water.splinePoints[1].width = 4.0f;
    water.splinePoints[1].depth = 5.0f;
    water.splinePoints[1].flowSpeed = 4.0f;

    scene::Transform pose;
    pose.worldPosition = {100.0f, 50.0f, -20.0f};
    pose.worldScale = {-2.0f, 3.0f, 0.5f};
    ASSERT_TRUE(scene::PrepareWaterSplineSnapshot(water, pose));
    ASSERT_NE(water.splineSnapshot, nullptr);

    scene::WaterSplineSample splineSample;
    ASSERT_TRUE(scene::TrySampleWaterSpline(*water.splineSnapshot, 90.0f, -20.0f, splineSample));
    EXPECT_NEAR(splineSample.baseHeight, 53.0f, testkit::kTolerance);
    EXPECT_NEAR(splineSample.baseHeightOffset, 3.0f, testkit::kTolerance);
    EXPECT_NEAR(splineSample.depth, 3.5f, testkit::kTolerance);
    EXPECT_NEAR(splineSample.worldVelocity.x, -3.0f, testkit::kTolerance);
    EXPECT_NEAR(splineSample.worldVelocity.z, 0.0f, testkit::kTolerance);
    EXPECT_GT(water.splineSnapshot->maximumWorldEdgeLength, 0.0f);

    scene::WaterSurfaceHeightSample height;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {90.0f, 51.0f, -20.0f}, 0.0f, height));
    EXPECT_TRUE(height.insideSurface);
    EXPECT_TRUE(height.underwater);
    EXPECT_NEAR(height.position.y, 53.0f, testkit::kTolerance);
    EXPECT_NEAR(height.waterDepthLimit, 3.5f, testkit::kTolerance);
    EXPECT_NEAR(height.depth, 2.0f, testkit::kTolerance);
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {90.0f, 49.0f, -20.0f}, 0.0f, height));
    EXPECT_FALSE(height.underwater);

    scene::WaterSurfaceSample surface;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceWorld(water, pose, {}, {90.0f, 51.0f, -20.0f}, 0.0f, surface));
    EXPECT_GT(surface.normal.x, 0.2f);
    EXPECT_GT(surface.normal.y, 0.9f);
    scene::ActiveFlowField authoredField{};
    authoredField.position = {90.0f, 53.0f, -20.0f};
    authoredField.radius = 0.0f;
    authoredField.direction = {0.0f, 0.0f, 1.0f};
    authoredField.strength = 2.0f;
    authoredField.type = scene::FlowFieldType::Uniform;
    authoredField.falloffPower = 2.0f;
    authoredField.channels = 0xffffffffu;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceWorld(water, pose, {authoredField},
        {90.0f, 51.0f, -20.0f}, 0.0f, surface));
    EXPECT_VEC3_NEAR(surface.flowVelocity, math::Vector3(-3.0f, 0.0f, 2.0f), testkit::kTolerance);
}

TEST_F(WaterSplineTest, TriangulatesConcaveLakeAndRejectsDegenerateInputWithoutChangingOutput)
{
    const std::array<math::Vector2, 6> outline{{
        {0.0f, 0.0f}, {4.0f, 0.0f}, {4.0f, 1.0f},
        {1.0f, 1.0f}, {1.0f, 4.0f}, {0.0f, 4.0f},
    }};
    std::vector<scene::WaterSplinePoint> points(outline.size());
    for (std::size_t i = 0; i < outline.size(); ++i) {
        points[i].id = static_cast<std::uint32_t>(i + 1);
        points[i].localPosition = {outline[i].x, 2.0f, outline[i].y};
        points[i].depth = 7.0f;
    }
    scene::Transform pose;
    pose.worldPosition.y = 10.0f;
    scene::WaterSplineSnapshot snapshot;
    ASSERT_TRUE(scene::BuildWaterSplineSnapshot(scene::WaterSurfaceShape::SPLINE_LAKE,
        points, 1, pose, snapshot, 5.0f, 100.0f));
    EXPECT_EQ(snapshot.outlineIndices.size(), outline.size());
    EXPECT_EQ(snapshot.triangles.size(), outline.size() - 2);

    scene::WaterSplineSample sample;
    EXPECT_TRUE(scene::TrySampleWaterSpline(snapshot, 0.5f, 0.5f, sample));
    EXPECT_NEAR(sample.baseHeight, 12.0f, testkit::kTolerance);
    EXPECT_NEAR(sample.depth, 5.0f, testkit::kTolerance);
    EXPECT_FALSE(scene::TrySampleWaterSpline(snapshot, 2.0f, 2.0f, sample));

    std::array<scene::WaterSplinePoint, 3> degenerate{};
    degenerate[0].id = 1;
    degenerate[0].localPosition = {0.0f, 0.0f, 0.0f};
    degenerate[1].id = 2;
    degenerate[1].localPosition = {1.0f, 0.0f, 0.0f};
    degenerate[2].id = 3;
    degenerate[2].localPosition = {2.0f, 0.0f, 0.0f};
    scene::WaterSplineSnapshot unchanged;
    unchanged.signature = 42;
    unchanged.vertices.push_back({});
    EXPECT_FALSE(scene::BuildWaterSplineSnapshot(scene::WaterSurfaceShape::SPLINE_LAKE,
        degenerate, 1, pose, unchanged, 5.0f, 100.0f));
    EXPECT_EQ(unchanged.signature, 42u);
    EXPECT_EQ(unchanged.vertices.size(), 1u);

    std::vector<scene::WaterSplinePoint> overBudgetLake(33);
    std::vector<scene::WaterSplinePoint> overBudgetRiver(33);
    for (std::size_t i = 0; i < overBudgetLake.size(); ++i) {
        const float angle = math::TWO_PI * static_cast<float>(i) / static_cast<float>(overBudgetLake.size());
        overBudgetLake[i].id = static_cast<std::uint32_t>(i + 1);
        overBudgetLake[i].localPosition = {std::cos(angle), 0.0f, std::sin(angle)};
        overBudgetRiver[i].id = static_cast<std::uint32_t>(i + 1);
        overBudgetRiver[i].localPosition = {static_cast<float>(i), 0.0f, 0.0f};
        overBudgetRiver[i].width = 1.0f;
    }
    EXPECT_FALSE(scene::BuildWaterSplineSnapshot(scene::WaterSurfaceShape::SPLINE_LAKE,
        overBudgetLake, 64, pose, unchanged, 5.0f, 100.0f));
    EXPECT_FALSE(scene::BuildWaterSplineSnapshot(scene::WaterSurfaceShape::SPLINE_RIVER,
        overBudgetRiver, 64, pose, unchanged, 5.0f, 100.0f));
    EXPECT_EQ(unchanged.signature, 42u);
    EXPECT_EQ(unchanged.vertices.size(), 1u);
}

TEST_F(WaterSplineTest, RepreparesAfterWorldPoseChangesAndSupportsDrySplineSurfaces)
{
    scene::WaterComponent water;
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.buoyancyDepth = 0.0f;
    water.splineSamplesPerSegment = 2;
    water.splineMeshCellSize = 100.0f;
    water.waveCount = 0;
    water.splinePoints.resize(2);
    water.splinePoints[0].id = 1;
    water.splinePoints[0].localPosition = {0.0f, 0.0f, 0.0f};
    water.splinePoints[0].width = 2.0f;
    water.splinePoints[0].depth = 0.0f;
    water.splinePoints[1].id = 2;
    water.splinePoints[1].localPosition = {10.0f, 0.0f, 0.0f};
    water.splinePoints[1].width = 2.0f;
    water.splinePoints[1].depth = 5.0f;

    scene::Transform pose;
    ASSERT_TRUE(scene::PrepareWaterSplineSnapshot(water, pose));
    const std::uint64_t firstSignature = water.splineSnapshot->signature;
    pose.worldPosition.x = 50.0f;
    ASSERT_TRUE(scene::PrepareWaterSplineSnapshot(water, pose));
    ASSERT_NE(water.splineSnapshot->signature, firstSignature);
    scene::WaterSplineSample splineSample;
    ASSERT_TRUE(scene::TrySampleWaterSpline(*water.splineSnapshot, 55.0f, 0.0f, splineSample));

    scene::WaterSurfaceHeightSample height;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {55.0f, -1.0f, 0.0f}, 0.0f, height));
    EXPECT_TRUE(height.insideSurface);
    EXPECT_FALSE(height.underwater);
    EXPECT_EQ(height.waterDepthLimit, 0.0f);
}

TEST_F(WaterSplineTest, InvalidSelectedSplineDoesNotFallBackToRectangle)
{
    scene::WaterComponent water;
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_LAKE;
    scene::Transform pose;
    EXPECT_FALSE(scene::IsWaterSurfaceSupported(water, pose));
    scene::WaterSurfaceHeightSample sample;
    EXPECT_FALSE(scene::TrySampleWaterSurfaceHeightWorld(water, pose, {}, 0.0f, sample));
    EXPECT_FALSE(scene::PrepareWaterSplineSnapshot(water, pose));
    EXPECT_EQ(water.splineSnapshot, nullptr);
}

TEST_F(WaterSplineTest, SimulationSurfacePublicationStaysStableWhileLiveGeometryChanges)
{
    scene::Scene world;
    physics::World physicsWorld;
    scene::WaterSystem waterSystem;
    SystemContext pausedContext{world, physicsWorld, nullptr, nullptr, 0.0f, 0.0f, false, true};
    auto& object = world.CreateGameObject("Water");
    auto& water = world.AddComponent<scene::WaterComponent>(object.GetID(), scene::WaterComponent{});
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.splineMeshCellSize = 100.0f;
    water.splineSamplesPerSegment = 2;
    water.splinePoints.resize(2);
    water.splinePoints[0].id = 1;
    water.splinePoints[0].localPosition = {0.0f, 0.0f, 0.0f};
    water.splinePoints[0].width = 2.0f;
    water.splinePoints[1].id = 2;
    water.splinePoints[1].localPosition = {10.0f, 0.0f, 0.0f};
    water.splinePoints[1].width = 2.0f;

    waterSystem.Update(pausedContext);
    ASSERT_NE(water.simulationSplineSnapshot, nullptr);
    const auto simulationSnapshot = water.simulationSplineSnapshot;
    const std::uint64_t publishedSignature = simulationSnapshot->signature;
    ASSERT_TRUE(water.simulationReadEnabled);

    water.splinePoints[1].localPosition.x = 20.0f;
    scene::PrepareWaterSceneSurfaces(world);
    ASSERT_NE(water.splineSnapshot, nullptr);
    EXPECT_NE(water.splineSnapshot->signature, publishedSignature);
    EXPECT_EQ(scene::WaterSurfaceGeometry(water, scene::WaterSurfaceReadView::SIMULATION),
        simulationSnapshot.get());
    EXPECT_EQ(scene::WaterSurfaceGeometry(water, scene::WaterSurfaceReadView::SIMULATION)->signature,
        publishedSignature);
    EXPECT_EQ(scene::WaterSurfaceGeometry(water, scene::WaterSurfaceReadView::LIVE_QUERY)->signature,
        water.splineSnapshot->signature);

    auto& unrelated = world.CreateGameObject("Unrelated Water");
    auto& unrelatedWater = world.AddComponent<scene::WaterComponent>(unrelated.GetID(), scene::WaterComponent{});
    waterSystem.Update(pausedContext);
    const auto* publishedWater = world.GetComponent<scene::WaterComponent>(object.GetID());
    ASSERT_NE(publishedWater, nullptr);
    ASSERT_TRUE(unrelatedWater.simulationSurfacePublished);
    EXPECT_EQ(scene::WaterSurfaceGeometry(*publishedWater, scene::WaterSurfaceReadView::SIMULATION),
        simulationSnapshot.get());
    EXPECT_EQ(scene::WaterSurfaceGeometry(*publishedWater, scene::WaterSurfaceReadView::SIMULATION)->signature,
        publishedSignature);

    auto* mutablePublishedWater = world.GetComponent<scene::WaterComponent>(object.GetID());
    ASSERT_NE(mutablePublishedWater, nullptr);
    mutablePublishedWater->simulationReadEnabled = false;
    EXPECT_EQ(scene::WaterSurfaceGeometry(*mutablePublishedWater, scene::WaterSurfaceReadView::SIMULATION)->signature,
        mutablePublishedWater->splineSnapshot->signature);
}

TEST_F(WaterSplineTest, RejectsFiniteGeometryWhoseSharedLocalExtentWouldOverflow)
{
    std::array<scene::WaterSplinePoint, 2> points{};
    points[0].id = 1;
    points[0].localPosition = {5.0e37f, 0.0f, 0.0f};
    points[0].width = 3.0e38f;
    points[1].id = 2;
    points[1].localPosition = {5.0e37f, 0.0f, 1.0e35f};
    points[1].width = 3.0e38f;
    scene::Transform pose;
    pose.worldScale.x = 0.0001f;
    scene::WaterSplineSnapshot unchanged;
    unchanged.signature = 42;
    unchanged.vertices.push_back({});
    EXPECT_FALSE(scene::BuildWaterSplineSnapshot(scene::WaterSurfaceShape::SPLINE_RIVER,
        points, 1, pose, unchanged, 5.0f, 100.0f));
    EXPECT_EQ(unchanged.signature, 42u);
    EXPECT_EQ(unchanged.vertices.size(), 1u);

    points[0].localPosition = {0.0f, 0.0f, 0.0f};
    points[0].width = 1.0e35f;
    points[1].localPosition = {5.6e37f, 0.0f, 0.0f};
    points[1].width = 1.0e35f;
    pose.worldScale.x = 3.2f;
    EXPECT_FALSE(scene::BuildWaterSplineSnapshot(scene::WaterSurfaceShape::SPLINE_RIVER,
        points, 1, pose, unchanged, 5.0f, 3.0e38f));
    EXPECT_EQ(unchanged.signature, 42u);
    EXPECT_EQ(unchanged.vertices.size(), 1u);
}

TEST_F(WaterSplineTest, ReflectionCloneKeepsSplineFieldsAndMaximumReflectableStableID)
{
    scene::WaterComponent source;
    source.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    source.splineSamplesPerSegment = 12;
    source.splineMeshCellSize = 0.75f;
    source.splinePoints.resize(2);
    source.splinePoints[0].id = scene::WATER_SPLINE_MAX_POINT_ID;
    source.splinePoints[0].localPosition = {1.0f, 2.0f, 3.0f};
    source.splinePoints[0].width = 4.0f;
    source.splinePoints[0].depth = 5.0f;
    source.splinePoints[0].flowSpeed = -6.0f;
    source.splinePoints[1].id = 2;
    source.splinePoints[1].localPosition = {7.0f, 8.0f, 9.0f};
    source.splinePoints[1].width = 10.0f;
    source.splinePoints[1].depth = 11.0f;
    source.splinePoints[1].flowSpeed = 12.0f;

    const auto table = scene::SerializeReflected(source);
    scene::WaterComponent clone;
    scene::DeserializeReflected(table, clone);
    EXPECT_EQ(clone.surfaceShape, source.surfaceShape);
    EXPECT_EQ(clone.splineSamplesPerSegment, source.splineSamplesPerSegment);
    EXPECT_FLOAT_EQ(clone.splineMeshCellSize, source.splineMeshCellSize);
    ASSERT_EQ(clone.splinePoints.size(), source.splinePoints.size());
    EXPECT_EQ(clone.splinePoints[0].id, source.splinePoints[0].id);
    EXPECT_VEC3_NEAR(clone.splinePoints[0].localPosition, source.splinePoints[0].localPosition,
        testkit::kTolerance);
    EXPECT_FLOAT_EQ(clone.splinePoints[0].width, source.splinePoints[0].width);
    EXPECT_FLOAT_EQ(clone.splinePoints[0].depth, source.splinePoints[0].depth);
    EXPECT_FLOAT_EQ(clone.splinePoints[0].flowSpeed, source.splinePoints[0].flowSpeed);
    EXPECT_EQ(clone.splinePoints[1].id, source.splinePoints[1].id);
}

TEST_F(WaterSplineTest, PointObjectReflectionKeepsOrderedBindingsWithoutInlinePositionCopies)
{
    scene::WaterComponent waterSource;
    waterSource.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    waterSource.splinePoints.push_back({77, {9.0f, 4.0f, 3.0f}, 1.0f, 2.0f, 3.0f});
    waterSource.pointBindings.resize(2);
    waterSource.pointBindings[0].id = 12;
    waterSource.pointBindings[0].target.guid = "guid:first-water-point";
    waterSource.pointBindings[0].width = 4.0f;
    waterSource.pointBindings[0].depth = 5.0f;
    waterSource.pointBindings[0].flowSpeed = -6.0f;
    waterSource.pointBindings[1].id = 3;
    waterSource.pointBindings[1].target.guid = "guid:second-water-point";
    waterSource.pointBindings[1].width = 7.0f;
    waterSource.pointBindings[1].depth = 8.0f;
    waterSource.pointBindings[1].flowSpeed = 9.0f;

    const auto waterTable = scene::SerializeReflected(waterSource);
    scene::WaterComponent waterClone;
    scene::DeserializeReflected(waterTable, waterClone);
    EXPECT_EQ(waterClone.pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    EXPECT_TRUE(waterClone.splinePoints.empty());
    ASSERT_EQ(waterClone.pointBindings.size(), 2u);
    EXPECT_EQ(waterClone.pointBindings[0].id, 12u);
    EXPECT_EQ(waterClone.pointBindings[0].target.guid, "guid:first-water-point");
    EXPECT_FLOAT_EQ(waterClone.pointBindings[0].width, 4.0f);
    EXPECT_FLOAT_EQ(waterClone.pointBindings[0].depth, 5.0f);
    EXPECT_FLOAT_EQ(waterClone.pointBindings[0].flowSpeed, -6.0f);
    EXPECT_EQ(waterClone.pointBindings[1].id, 3u);
    EXPECT_EQ(waterClone.pointBindings[1].target.guid, "guid:second-water-point");

    scene::SplineComponent splineSource;
    splineSource.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    splineSource.points.push_back({4.0f, 5.0f, 6.0f});
    splineSource.pointBindings.resize(2);
    splineSource.pointBindings[0].id = 21;
    splineSource.pointBindings[0].target.guid = "guid:first-spline-point";
    splineSource.pointBindings[1].id = 8;
    splineSource.pointBindings[1].target.guid = "guid:second-spline-point";

    const auto splineTable = scene::SerializeReflected(splineSource);
    scene::SplineComponent splineClone;
    scene::DeserializeReflected(splineTable, splineClone);
    EXPECT_EQ(splineClone.pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    EXPECT_TRUE(splineClone.points.empty());
    ASSERT_EQ(splineClone.pointBindings.size(), 2u);
    EXPECT_EQ(splineClone.pointBindings[0].id, 21u);
    EXPECT_EQ(splineClone.pointBindings[0].target.guid, "guid:first-spline-point");
    EXPECT_EQ(splineClone.pointBindings[1].id, 8u);
    EXPECT_EQ(splineClone.pointBindings[1].target.guid, "guid:second-spline-point");
}

TEST_F(WaterSplineTest, StaticBakesRemainValidWhenCloneRemapsPointGuidsAtTheSameNumericPositions)
{
    scene::Scene world;
    auto& sourceOwner = world.CreateGameObject("Source Owner");
    auto& cloneOwner = world.CreateGameObject("Clone Owner");
    sourceOwner.transform.position = {12.0f, 3.0f, -8.0f};
    cloneOwner.transform.position = {-120.0f, 1.0f, 83.0f};

    auto& sourcePointA = world.CreateGameObject("Source Point A");
    auto& sourcePointB = world.CreateGameObject("Source Point B");
    auto& clonePointA = world.CreateGameObject("Clone Point A");
    auto& clonePointB = world.CreateGameObject("Clone Point B");
    sourcePointA.SetParent(sourceOwner);
    sourcePointB.SetParent(sourceOwner);
    clonePointA.SetParent(cloneOwner);
    clonePointB.SetParent(cloneOwner);
    ASSERT_EQ(sourcePointA.GetParent(), &sourceOwner);
    ASSERT_EQ(sourcePointB.GetParent(), &sourceOwner);
    ASSERT_EQ(clonePointA.GetParent(), &cloneOwner);
    ASSERT_EQ(clonePointB.GetParent(), &cloneOwner);
    sourcePointA.transform.position = clonePointA.transform.position = {-4.0f, 1.0f, 2.0f};
    sourcePointB.transform.position = clonePointB.transform.position = {6.0f, 2.0f, -3.0f};
    sourcePointA.AddComponent<scene::TargetPointComponent>();
    sourcePointB.AddComponent<scene::TargetPointComponent>();
    clonePointA.AddComponent<scene::TargetPointComponent>();
    clonePointB.AddComponent<scene::TargetPointComponent>();
    scene::FlushWorldTransforms(world);
    ASSERT_NE(sourcePointA.instanceId, clonePointA.instanceId);

    auto& sourceWater = sourceOwner.AddComponent<scene::WaterComponent>();
    auto& cloneWater = cloneOwner.AddComponent<scene::WaterComponent>();
    const auto configureWater = [](scene::WaterComponent& water,
        const scene::GameObject& pointA, const scene::GameObject& pointB) {
        water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
        water.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
        water.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;
        water.pointBindings.resize(2);
        water.pointBindings[0].id = 18;
        water.pointBindings[0].target.guid = pointA.instanceId;
        water.pointBindings[0].width = 3.0f;
        water.pointBindings[0].depth = 4.0f;
        water.pointBindings[0].flowSpeed = -2.0f;
        water.pointBindings[1].id = 7;
        water.pointBindings[1].target.guid = pointB.instanceId;
        water.pointBindings[1].width = 5.0f;
        water.pointBindings[1].depth = 6.0f;
        water.pointBindings[1].flowSpeed = 1.5f;
    };
    configureWater(sourceWater, sourcePointA, sourcePointB);
    configureWater(cloneWater, clonePointA, clonePointB);
    const auto sourceWaterBake = scene::ResolveWaterControlPointAuthoring(
        world, sourceOwner.GetID(), sourceWater, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_TRUE(sourceWaterBake.valid);
    sourceWater.bakedSplinePoints = sourceWaterBake.points;
    sourceWater.bakedInputSignature = sourceWaterBake.inputSignature;
    sourceWater.bakedSourceMode = sourceWater.pointSourceMode;
    sourceWater.bakedSurfaceShape = sourceWaterBake.surfaceShape;
    sourceWater.bakedLakePlaneHeight = sourceWaterBake.lakePlaneHeight;
    sourceWater.bakedAdapterVersion = scene::CONTROL_POINT_ADAPTER_VERSION;
    sourceWater.bakeValid = true;
    cloneWater.bakedSplinePoints = sourceWater.bakedSplinePoints;
    cloneWater.bakedInputSignature = sourceWater.bakedInputSignature;
    cloneWater.bakedSourceMode = sourceWater.bakedSourceMode;
    cloneWater.bakedSurfaceShape = sourceWater.bakedSurfaceShape;
    cloneWater.bakedLakePlaneHeight = sourceWater.bakedLakePlaneHeight;
    cloneWater.bakedAdapterVersion = sourceWater.bakedAdapterVersion;
    cloneWater.bakeValid = true;
    const auto cloneWaterAuthoring = scene::ResolveWaterControlPointAuthoring(
        world, cloneOwner.GetID(), cloneWater, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_TRUE(cloneWaterAuthoring.valid);
    EXPECT_EQ(cloneWaterAuthoring.inputSignature, sourceWaterBake.inputSignature);
    EXPECT_FALSE(scene::ResolveWaterControlPoints(world, cloneOwner.GetID(), cloneWater).bakeStale);

    auto& sourceSpline = sourceOwner.AddComponent<scene::SplineComponent>();
    auto& cloneSpline = cloneOwner.AddComponent<scene::SplineComponent>();
    const auto configureSpline = [](scene::SplineComponent& spline,
        const scene::GameObject& pointA, const scene::GameObject& pointB) {
        spline.closed = true;
        spline.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
        spline.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;
        spline.pointBindings.resize(2);
        spline.pointBindings[0].id = 31;
        spline.pointBindings[0].target.guid = pointA.instanceId;
        spline.pointBindings[1].id = 12;
        spline.pointBindings[1].target.guid = pointB.instanceId;
    };
    configureSpline(sourceSpline, sourcePointA, sourcePointB);
    configureSpline(cloneSpline, clonePointA, clonePointB);
    const auto sourceSplineBake = scene::ResolveSplinePointAuthoring(
        world, sourceOwner.GetID(), sourceSpline, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_TRUE(sourceSplineBake.valid);
    sourceSpline.bakedPoints = sourceSplineBake.points;
    sourceSpline.bakedInputSignature = sourceSplineBake.inputSignature;
    sourceSpline.bakedSourceMode = sourceSpline.pointSourceMode;
    sourceSpline.bakedAdapterVersion = scene::CONTROL_POINT_ADAPTER_VERSION;
    sourceSpline.bakedClosed = sourceSpline.closed;
    sourceSpline.bakeValid = true;
    cloneSpline.bakedPoints = sourceSpline.bakedPoints;
    cloneSpline.bakedInputSignature = sourceSpline.bakedInputSignature;
    cloneSpline.bakedSourceMode = sourceSpline.bakedSourceMode;
    cloneSpline.bakedAdapterVersion = sourceSpline.bakedAdapterVersion;
    cloneSpline.bakedClosed = sourceSpline.bakedClosed;
    cloneSpline.bakeValid = true;
    const auto cloneSplineAuthoring = scene::ResolveSplinePointAuthoring(
        world, cloneOwner.GetID(), cloneSpline, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_TRUE(cloneSplineAuthoring.valid);
    EXPECT_EQ(cloneSplineAuthoring.inputSignature, sourceSplineBake.inputSignature);
    EXPECT_FALSE(scene::ResolveSplinePoints(world, cloneOwner.GetID(), cloneSpline).bakeStale);
    EXPECT_TRUE(scene::ValidateStaticControlPointBakes(world));
}

TEST_F(WaterSplineTest, StaticBakeValidationRejectsChangedWaterAndSplineAuthoring)
{
    scene::Scene world;
    auto& waterObject = world.CreateGameObject("Baked River");
    auto& water = world.AddComponent<scene::WaterComponent>(waterObject.GetID(), scene::WaterComponent{});
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.splinePoints = {
        {1, {0.0f, 0.0f, 0.0f}, 2.0f, 1.0f, 0.5f},
        {2, {10.0f, 0.0f, 0.0f}, 2.0f, 1.0f, 0.5f},
    };
    const auto waterAuthoring = scene::ResolveWaterControlPointAuthoring(
        world, waterObject.GetID(), water, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_TRUE(waterAuthoring.valid);
    water.bakedSplinePoints = waterAuthoring.points;
    water.bakedInputSignature = waterAuthoring.inputSignature;
    water.bakedSourceMode = water.pointSourceMode;
    water.bakedSurfaceShape = waterAuthoring.surfaceShape;
    water.bakedLakePlaneHeight = waterAuthoring.lakePlaneHeight;
    water.bakedAdapterVersion = scene::CONTROL_POINT_ADAPTER_VERSION;
    water.bakeValid = true;
    water.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;

    auto& splineObject = world.CreateGameObject("Baked Spline");
    auto& spline = world.AddComponent<scene::SplineComponent>(splineObject.GetID(), scene::SplineComponent{});
    spline.closed = true;
    spline.points = {{0.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f}};
    const auto splineAuthoring = scene::ResolveSplinePointAuthoring(
        world, splineObject.GetID(), spline, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_TRUE(splineAuthoring.valid);
    spline.bakedPoints = splineAuthoring.points;
    spline.bakedInputSignature = splineAuthoring.inputSignature;
    spline.bakedSourceMode = spline.pointSourceMode;
    spline.bakedAdapterVersion = scene::CONTROL_POINT_ADAPTER_VERSION;
    spline.bakedClosed = spline.closed;
    spline.bakeValid = true;
    spline.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;

    EXPECT_TRUE(scene::ValidateStaticControlPointBakes(world));
    water.bakedSplinePoints[1].localPosition.x = 12.0f;
    EXPECT_FALSE(scene::ValidateStaticControlPointBakes(world));
    water.bakedSplinePoints[1].localPosition.x = 10.0f;
    spline.bakedPoints[1].localPosition.x = 8.0f;
    EXPECT_FALSE(scene::ValidateStaticControlPointBakes(world));
    spline.bakedPoints[1].localPosition.x = 4.0f;
    spline.bakedClosed = false;
    EXPECT_FALSE(scene::ValidateStaticControlPointBakes(world));
    spline.bakedClosed = true;
    EXPECT_TRUE(scene::ValidateStaticControlPointBakes(world));
    ASSERT_TRUE(scene::PrepareWaterSplineSnapshot(world, waterObject.GetID(), water,
        waterObject.transform));
    const auto bakedGeometry = water.splineSnapshot;
    ASSERT_NE(bakedGeometry, nullptr);
    water.splinePoints[1].localPosition.x = 12.0f;
    EXPECT_FALSE(scene::ValidateStaticControlPointBakes(world));
    ASSERT_TRUE(scene::PrepareWaterSplineSnapshot(world, waterObject.GetID(), water,
        waterObject.transform));
    EXPECT_EQ(water.splineSnapshot, bakedGeometry);
    water.splinePoints[1].localPosition.x = 10.0f;
    water.surfaceShape = scene::WaterSurfaceShape::RECTANGLE;
    EXPECT_FALSE(scene::ValidateStaticControlPointBakes(world));
    EXPECT_EQ(scene::WaterSurfaceEffectiveShape(water), scene::WaterSurfaceShape::SPLINE_RIVER);
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    spline.closed = false;
    EXPECT_FALSE(scene::ValidateStaticControlPointBakes(world));
    const auto staleSpline = scene::ResolveSplinePoints(world, splineObject.GetID(), spline);
    EXPECT_TRUE(staleSpline.valid);
    EXPECT_TRUE(staleSpline.bakeStale);
    EXPECT_TRUE(staleSpline.closed);
}

TEST_F(WaterSplineTest, StaticRuntimeReadsNumericBakesWithoutResolvingMovedOrMissingMarkers)
{
    scene::Scene world;
    auto& owner = world.CreateGameObject("Baked owner");
    const scene::EntityID ownerId = owner.GetID();
    auto& first = world.CreateGameObject("First point");
    const scene::EntityID firstId = first.GetID();
    first.SetParent(owner);
    auto& second = world.CreateGameObject("Second point");
    const scene::EntityID secondId = second.GetID();
    second.SetParent(owner);
    second.transform.position = {10.0f, 0.0f, 0.0f};
    world.AddComponent<scene::TargetPointComponent>(firstId, {});
    world.AddComponent<scene::TargetPointComponent>(secondId, {});
    scene::FlushWorldTransforms(world);

    auto& water = world.AddComponent<scene::WaterComponent>(ownerId, {});
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    water.pointBindings = {
        {1, {first.instanceId}, 2.0f, 1.0f, 0.5f},
        {2, {second.instanceId}, 2.0f, 1.0f, 0.5f},
    };
    const auto waterBake = scene::ResolveWaterControlPointAuthoring(
        world, ownerId, water, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_TRUE(waterBake.valid);
    water.bakedSplinePoints = waterBake.points;
    water.bakedInputSignature = waterBake.inputSignature;
    water.bakedSourceMode = water.pointSourceMode;
    water.bakedSurfaceShape = waterBake.surfaceShape;
    water.bakedLakePlaneHeight = waterBake.lakePlaneHeight;
    water.bakedAdapterVersion = scene::CONTROL_POINT_ADAPTER_VERSION;
    water.bakeValid = true;
    water.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;

    auto& spline = world.AddComponent<scene::SplineComponent>(ownerId, {});
    spline.pointSourceMode = scene::ControlPointSourceMode::POINT_OBJECTS;
    spline.pointBindings = {{1, {first.instanceId}}, {2, {second.instanceId}}};
    const auto splineBake = scene::ResolveSplinePointAuthoring(
        world, ownerId, spline, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_TRUE(splineBake.valid);
    spline.bakedPoints = splineBake.points;
    spline.bakedInputSignature = splineBake.inputSignature;
    spline.bakedSourceMode = spline.pointSourceMode;
    spline.bakedClosed = splineBake.closed;
    spline.bakedAdapterVersion = scene::CONTROL_POINT_ADAPTER_VERSION;
    spline.bakeValid = true;
    spline.pointRuntimeMode = scene::ControlPointRuntimeMode::BAKED_STATIC;
    ASSERT_TRUE(scene::ValidateStaticControlPointBakes(world));

    const auto clearReferenceCaches = [&] {
        for (auto& binding : water.pointBindings) {
            binding.target.id = scene::EntityID::INVALID;
            binding.target.sceneGeneration = 0;
        }
        for (auto& binding : spline.pointBindings) {
            binding.target.id = scene::EntityID::INVALID;
            binding.target.sceneGeneration = 0;
        }
    };
    clearReferenceCaches();
    second.transform.position.x = 20.0f;
    scene::FlushWorldTransforms(world);
    const auto runtimeWater = scene::ResolveWaterControlPoints(
        world, ownerId, water, scene::ControlPointReadContext::RUNTIME);
    const auto runtimeSpline = scene::ResolveSplinePoints(
        world, ownerId, spline, scene::ControlPointReadContext::RUNTIME);
    ASSERT_TRUE(runtimeWater.valid);
    ASSERT_TRUE(runtimeSpline.valid);
    EXPECT_EQ(runtimeWater.inputSignature, waterBake.inputSignature);
    EXPECT_VEC3_NEAR(runtimeWater.points[1].localPosition, waterBake.points[1].localPosition,
        testkit::kTolerance);
    EXPECT_VEC3_NEAR(runtimeSpline.points[1].localPosition, splineBake.points[1].localPosition,
        testkit::kTolerance);
    for (const auto& binding : water.pointBindings)
        EXPECT_EQ(binding.target.id, scene::EntityID::INVALID);
    for (const auto& binding : spline.pointBindings)
        EXPECT_EQ(binding.target.id, scene::EntityID::INVALID);
    EXPECT_FALSE(scene::ValidateStaticControlPointBakes(world));

    clearReferenceCaches();
    ASSERT_TRUE(world.DestroyGameObject(secondId));
    EXPECT_TRUE(scene::ResolveWaterControlPoints(
        world, ownerId, water, scene::ControlPointReadContext::RUNTIME).valid);
    EXPECT_TRUE(scene::ResolveSplinePoints(
        world, ownerId, spline, scene::ControlPointReadContext::RUNTIME).valid);
    water.simulationReadEnabled = true;
    ASSERT_TRUE(scene::PrepareWaterSplineSnapshot(world, ownerId, water, owner.transform));
    for (const auto& binding : water.pointBindings)
        EXPECT_EQ(binding.target.id, scene::EntityID::INVALID);
    EXPECT_FALSE(scene::ValidateStaticControlPointBakes(world));

    water.bakedSplinePoints[0].width = -1.0f;
    spline.bakedPoints[0].id = 0;
    EXPECT_FALSE(scene::ResolveWaterControlPoints(
        world, ownerId, water, scene::ControlPointReadContext::RUNTIME).valid);
    EXPECT_FALSE(scene::ResolveSplinePoints(
        world, ownerId, spline, scene::ControlPointReadContext::RUNTIME).valid);
}

TEST_F(WaterSplineTest, TomlIntReflectionRejectsOutOfRangeValuesAndPreservesSignedMasks)
{
    toml::table table;
    table.insert_or_assign("tooLarge", std::int64_t{ 4294967297ll });
    table.insert_or_assign("tooSmall", std::int64_t{ -2147483649ll });
    table.insert_or_assign("minimum", std::int64_t{ -2147483648ll });
    table.insert_or_assign("maximum", std::int64_t{ 2147483647ll });
    table.insert_or_assign("negativeMask", std::int64_t{ -1 });
    table.insert_or_assign("integerList", toml::array{
        std::int64_t{ 4294967297ll }, std::int64_t{ -2147483648ll }, std::int64_t{ -1 }});

    util::TomlReadReflector reader(table);
    int tooLarge = 9;
    int tooSmall = 9;
    int minimum = 0;
    int maximum = 0;
    int negativeMask = 0;
    reader.Field("tooLarge", tooLarge);
    reader.Field("tooSmall", tooSmall);
    reader.Field("minimum", minimum);
    reader.Field("maximum", maximum);
    reader.Field("negativeMask", negativeMask);

    EXPECT_EQ(tooLarge, (std::numeric_limits<int>::min)());
    EXPECT_EQ(tooSmall, (std::numeric_limits<int>::min)());
    EXPECT_EQ(minimum, (std::numeric_limits<int>::min)());
    EXPECT_EQ(maximum, (std::numeric_limits<int>::max)());
    EXPECT_EQ(negativeMask, -1);

    std::vector<int> integerList;
    reader.ListField("integerList", integerList);
    ASSERT_EQ(integerList.size(), 3u);
    EXPECT_EQ(integerList[0], (std::numeric_limits<int>::min)());
    EXPECT_EQ(integerList[1], (std::numeric_limits<int>::min)());
    EXPECT_EQ(integerList[2], -1);
}

TEST_F(WaterSplineTest, OversizedControlPointEnumsAndBindingIDsRemainInvalidAfterReflection)
{
    toml::array waterBindings;
    waterBindings.push_back(toml::table{{"id", std::int64_t{ 4294967297ll }}});
    toml::table waterTable;
    waterTable.insert_or_assign("pointSourceMode", std::int64_t{ 4294967297ll });
    waterTable.insert_or_assign("pointRuntimeMode", std::int64_t{ 4294967296ll });
    waterTable.insert_or_assign("pointBindings", std::move(waterBindings));

    scene::WaterComponent water;
    scene::DeserializeReflected(waterTable, water);
    EXPECT_NE(water.pointSourceMode, scene::ControlPointSourceMode::INLINE);
    EXPECT_NE(water.pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    EXPECT_NE(water.pointRuntimeMode, scene::ControlPointRuntimeMode::LIVE);
    EXPECT_NE(water.pointRuntimeMode, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_EQ(water.pointBindings.size(), 1u);
    EXPECT_EQ(water.pointBindings[0].id, 0u);

    toml::array splineBindings;
    splineBindings.push_back(toml::table{{"id", std::int64_t{ 4294967297ll }}});
    toml::table splineTable;
    splineTable.insert_or_assign("pointSourceMode", std::int64_t{ 4294967297ll });
    splineTable.insert_or_assign("pointRuntimeMode", std::int64_t{ 4294967296ll });
    splineTable.insert_or_assign("pointBindings", std::move(splineBindings));

    scene::SplineComponent spline;
    scene::DeserializeReflected(splineTable, spline);
    EXPECT_NE(spline.pointSourceMode, scene::ControlPointSourceMode::INLINE);
    EXPECT_NE(spline.pointSourceMode, scene::ControlPointSourceMode::POINT_OBJECTS);
    EXPECT_NE(spline.pointRuntimeMode, scene::ControlPointRuntimeMode::LIVE);
    EXPECT_NE(spline.pointRuntimeMode, scene::ControlPointRuntimeMode::BAKED_STATIC);
    ASSERT_EQ(spline.pointBindings.size(), 1u);
    EXPECT_EQ(spline.pointBindings[0].id, 0u);
}

} /// @note namespace fbzz::tests
