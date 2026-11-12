/// @file    WaterSplineMeshTests.cpp
/// @brief   内部細分化の水面属性・巨大原点での退化回避・波の逆写像法線を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Math/MathUtils.hpp>
#include <array>

namespace fbzz::tests {
class WaterSplineMeshTest : public testkit::Fixture {};

TEST_F(WaterSplineMeshTest, InteriorRefinementKeepsTheOriginalHeightDepthAndDirectedFlow)
{
    const std::array<scene::WaterSplinePoint, 2> points{{
        {1, {0.0f, 0.0f, 0.0f}, 12.0f, 2.0f, 1.0f},
        {2, {32.0f, 4.0f, 0.0f}, 12.0f, 6.0f, -1.0f}}};
    scene::Transform transform;
    scene::WaterSplineSnapshot coarse, fine;
    ASSERT_TRUE(scene::BuildWaterSplineSnapshot(scene::WaterSurfaceShape::SPLINE_RIVER,
        points, 2, transform, coarse, 10.0f, 100.0f));
    ASSERT_TRUE(scene::BuildWaterSplineSnapshot(scene::WaterSurfaceShape::SPLINE_RIVER,
        points, 2, transform, fine, 10.0f, 0.5f));
    EXPECT_GT(fine.vertices.size(), coarse.vertices.size());
    EXPECT_LE(fine.maximumWorldEdgeLength, 0.5f);
    EXPECT_FALSE(fine.meshQualityLimited);
    EXPECT_EQ(fine.outlineIndices, coarse.outlineIndices);
    for (const auto position : std::array<math::Vector2, 3>{{{3.0f, 1.0f}, {16.0f, 0.0f}, {25.0f, -2.0f}}}) {
        scene::WaterSplineSample before, after;
        ASSERT_TRUE(scene::TrySampleWaterSpline(coarse, position.x, position.y, before));
        ASSERT_TRUE(scene::TrySampleWaterSpline(fine, position.x, position.y, after));
        EXPECT_NEAR(after.baseHeightOffset, before.baseHeightOffset, testkit::kTolerance);
        EXPECT_NEAR(after.depth, before.depth, testkit::kTolerance);
        EXPECT_VEC3_NEAR(after.worldVelocity, before.worldVelocity, testkit::kTolerance);
    }
}

TEST_F(WaterSplineMeshTest, LargeWorldOriginKeepsTheLastValidMeshWhenMidpointsLosePrecision)
{
    const std::array<scene::WaterSplinePoint, 2> points{{
        {1, {0.0f, 0.0f, 0.0f}, 32.0f, 2.0f, 1.0f},
        {2, {32.0f, 4.0f, 0.0f}, 32.0f, 2.0f, 1.0f}}};
    scene::Transform transform;
    transform.worldPosition.x = 100000000.0f;
    scene::WaterSplineSnapshot snapshot;
    ASSERT_TRUE(scene::BuildWaterSplineSnapshot(scene::WaterSurfaceShape::SPLINE_RIVER,
        points, 1, transform, snapshot, 10.0f, 2.0f));
    EXPECT_TRUE(snapshot.meshQualityLimited);
    EXPECT_GT(snapshot.maximumWorldEdgeLength, 2.0f);
    scene::WaterSplineSample sample;
    ASSERT_TRUE(scene::TrySampleWaterSpline(snapshot, 100000016.0f, 0.0f, sample));
    EXPECT_NEAR(sample.baseHeightOffset, 2.0f, testkit::kTolerance);
    EXPECT_NEAR(sample.depth, 2.0f, testkit::kTolerance);
    for (const auto& triangle : snapshot.triangles) {
        const auto& a = snapshot.vertices[triangle.indices[0]].worldPosition;
        const auto& b = snapshot.vertices[triangle.indices[1]].worldPosition;
        const auto& c = snapshot.vertices[triangle.indices[2]].worldPosition;
        const double area = (static_cast<double>(b.x) - a.x) * (static_cast<double>(c.z) - a.z)
            - (static_cast<double>(b.z) - a.z) * (static_cast<double>(c.x) - a.x);
        EXPECT_NE(area, 0.0);
    }
}

TEST_F(WaterSplineMeshTest, LongReachDoesNotSubdivideAlreadyDenseUpstreamTriangles)
{
    const std::array<scene::WaterSplinePoint, 4> points{{
        {1, {0.0f, 0.0f, 0.0f}, 4.0f, 2.0f, 1.0f},
        {2, {2.0f, 0.0f, 0.0f}, 4.0f, 2.0f, 1.0f},
        {3, {4.0f, 0.0f, 0.0f}, 4.0f, 2.0f, 1.0f},
        {4, {84.0f, 0.0f, 0.0f}, 4.0f, 2.0f, 1.0f}}};
    scene::Transform transform;
    scene::WaterSplineSnapshot mesh;
    ASSERT_TRUE(scene::BuildWaterSplineSnapshot(scene::WaterSurfaceShape::SPLINE_RIVER,
        points, 1, transform, mesh, 10.0f, 5.0f));
    EXPECT_FALSE(mesh.meshQualityLimited);
    EXPECT_LE(mesh.maximumWorldEdgeLength, 5.0f);
    size_t upstreamVertices = 0;
    for (const auto& vertex : mesh.vertices) {
        if (vertex.worldPosition.x < 4.0f) ++upstreamVertices;
    }
    EXPECT_EQ(upstreamVertices, 4u);
}

TEST_F(WaterSplineMeshTest, SlopedRiverNormalIncludesTheHorizontalWaveInverseMap)
{
    scene::WaterComponent water;
    water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
    water.splineSamplesPerSegment = 1;
    water.splinePoints = {{1, {0.0f, 0.0f, 0.0f}, 4.0f, 3.0f, 0.0f},
                         {2, {10.0f, 5.0f, 0.0f}, 4.0f, 3.0f, 0.0f}};
    water.waveCount = 1;
    water.waves[0] = {{1.0f, 0.0f}, 0.3f, math::TWO_PI, 1.0f};
    water.waveGrouping = water.waveSpread = 0.0f;
    scene::Transform transform;
    ASSERT_TRUE(scene::PrepareWaterSplineSnapshot(water, transform));
    scene::WaterSurfaceSample sample;
    ASSERT_TRUE(scene::TrySampleWaterSurfaceWorld(water, transform, {},
        {math::PI * 0.5f, 0.0f, 0.0f}, 0.0f, sample));
    const auto expected = math::Vector3{-0.5f / 0.7f, 1.0f, 0.0f}.Normalized();
    EXPECT_VEC3_NEAR(sample.normal, expected, 0.005f);
}
} /// @note namespace fbzz::tests
