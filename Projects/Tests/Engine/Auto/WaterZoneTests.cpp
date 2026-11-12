/// @file    WaterZoneTests.cpp
/// @brief   接続水域の厳密な和集合・優先属性・河口連続性を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/LogSinks.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <Engine/Scene/WaterZoneSnapshot.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

class WaterZoneTest : public testkit::Fixture {};

std::shared_ptr<const scene::WaterSplineSnapshot> MakeRectangle(
    float minX, float maxX, float minZ, float maxZ, float worldBaseY,
    float heightOffset = 0.0f, float depth = 2.0f,
    math::Vector3 velocity = math::Vector3::ZERO)
{
    auto snapshot = std::make_shared<scene::WaterSplineSnapshot>();
    snapshot->shape = scene::WaterSurfaceShape::RECTANGLE;
    snapshot->worldBaseY = worldBaseY;
    const std::array<math::Vector2, 4> points{{
        {minX, minZ}, {maxX, minZ}, {maxX, maxZ}, {minX, maxZ},
    }};
    for (const math::Vector2& point : points) {
        scene::WaterSplineVertex vertex;
        vertex.localPosition = {point.x, heightOffset, point.y};
        vertex.worldPosition = {point.x, worldBaseY + heightOffset, point.y};
        vertex.worldVelocity = velocity;
        vertex.baseHeightOffset = heightOffset;
        vertex.depth = depth;
        snapshot->vertices.push_back(vertex);
    }
    snapshot->triangles = {{{0, 1, 2}, {}}, {{0, 2, 3}, {}}};
    snapshot->footprintTriangles = snapshot->triangles;
    if (!scene::FinalizeWaterSplineSnapshot(*snapshot, 1000.0f)) return {};
    return snapshot;
}

std::shared_ptr<const scene::WaterSplineSnapshot> MakeTransformedRectangle(
    const math::Vector3& center, float yawDegrees, const math::Vector3& scale, float halfExtent)
{
    auto snapshot = std::make_shared<scene::WaterSplineSnapshot>();
    snapshot->shape = scene::WaterSurfaceShape::RECTANGLE;
    snapshot->worldBaseY = center.y;
    const math::Quaternion rotation = math::Quaternion::FromAxisAngle(
        math::Vector3::UP, math::ToRad(yawDegrees));
    const std::array<math::Vector2, 4> points{{
        {-halfExtent, -halfExtent}, {halfExtent, -halfExtent},
        {halfExtent, halfExtent}, {-halfExtent, halfExtent},
    }};
    for (const math::Vector2& point : points) {
        const math::Vector3 local{point.x * scale.x, 0.0f, point.y * scale.z};
        scene::WaterSplineVertex vertex;
        vertex.localPosition = {point.x, 0.0f, point.y};
        vertex.worldPosition = center + rotation * local;
        vertex.depth = 2.0f;
        snapshot->vertices.push_back(vertex);
    }
    snapshot->triangles = {{{0, 1, 2}, {}}, {{0, 2, 3}, {}}};
    snapshot->footprintTriangles = snapshot->triangles;
    if (!scene::FinalizeWaterSplineSnapshot(*snapshot, 1000.0f)) return {};
    return snapshot;
}

std::shared_ptr<const scene::WaterSplineSnapshot> MakeQuad(
    const std::array<math::Vector2, 4>& points)
{
    auto snapshot = std::make_shared<scene::WaterSplineSnapshot>();
    snapshot->shape = scene::WaterSurfaceShape::RECTANGLE;
    for (const math::Vector2& point : points) {
        scene::WaterSplineVertex vertex;
        vertex.localPosition = {point.x, 0.0f, point.y};
        vertex.worldPosition = {point.x, 0.0f, point.y};
        vertex.depth = 2.0f;
        snapshot->vertices.push_back(vertex);
    }
    snapshot->triangles = {{{0, 1, 2}, {}}, {{0, 2, 3}, {}}};
    snapshot->footprintTriangles = snapshot->triangles;
    if (!scene::FinalizeWaterSplineSnapshot(*snapshot, 1000.0f)) return {};
    return snapshot;
}

std::shared_ptr<const scene::WaterSplineSnapshot> MakeTriangle(
    const std::array<math::Vector2, 3>& points)
{
    auto snapshot = std::make_shared<scene::WaterSplineSnapshot>();
    snapshot->shape = scene::WaterSurfaceShape::SPLINE_LAKE;
    for (const math::Vector2& point : points) {
        scene::WaterSplineVertex vertex;
        vertex.localPosition = {point.x, 0.0f, point.y};
        vertex.worldPosition = {point.x, 0.0f, point.y};
        vertex.depth = 2.0f;
        snapshot->vertices.push_back(vertex);
    }
    snapshot->triangles = {{{0, 1, 2}, {}}};
    snapshot->footprintTriangles = snapshot->triangles;
    if (!scene::FinalizeWaterSplineSnapshot(*snapshot, 1000.0f)) return {};
    return snapshot;
}

double CrossXZ(const math::Vector3& a, const math::Vector3& b, const math::Vector3& c)
{
    return (static_cast<double>(b.x) - a.x) * (static_cast<double>(c.z) - a.z)
        - (static_cast<double>(b.z) - a.z) * (static_cast<double>(c.x) - a.x);
}

double SurfaceArea(const scene::WaterSplineSnapshot& geometry)
{
    double area = 0.0;
    for (const auto& triangle : geometry.triangles) {
        const auto& a = geometry.vertices[triangle.indices[0]].worldPosition;
        const auto& b = geometry.vertices[triangle.indices[1]].worldPosition;
        const auto& c = geometry.vertices[triangle.indices[2]].worldPosition;
        area += std::abs(CrossXZ(a, b, c)) * 0.5;
    }
    return area;
}

bool ContainsPoint(const scene::WaterSplineSnapshot& geometry, float x, float z)
{
    const math::Vector3 point{x, 0.0f, z};
    for (const auto& triangle : geometry.triangles) {
        const auto& a = geometry.vertices[triangle.indices[0]].worldPosition;
        const auto& b = geometry.vertices[triangle.indices[1]].worldPosition;
        const auto& c = geometry.vertices[triangle.indices[2]].worldPosition;
        const double orientation = CrossXZ(a, b, c);
        const double sign = orientation >= 0.0 ? 1.0 : -1.0;
        if (sign * CrossXZ(a, b, point) >= -1.0e-5
            && sign * CrossXZ(b, c, point) >= -1.0e-5
            && sign * CrossXZ(c, a, point) >= -1.0e-5) return true;
    }
    return false;
}

bool UnionContains(std::span<const scene::WaterZoneSource> sources, float x, float z)
{
    for (const auto& source : sources)
        if (source.geometry && ContainsPoint(*source.geometry, x, z)) return true;
    return false;
}

std::vector<scene::WaterZoneSource> MakeSources(
    std::shared_ptr<const scene::WaterSplineSnapshot> owner, int ownerPriority,
    std::shared_ptr<const scene::WaterSplineSnapshot> member, int memberPriority)
{
    return {{std::move(owner), ownerPriority}, {std::move(member), memberPriority}};
}

} /// @note namespace

TEST_F(WaterZoneTest, BuildsOneNonOverlappingUnionAndSplitsPartialSharedEdges)
{
    const auto owner = MakeRectangle(0.0f, 4.0f, 0.0f, 4.0f, 0.0f);
    const auto member = MakeRectangle(4.0f, 6.0f, 1.0f, 3.0f, 0.0f);
    ASSERT_NE(owner, nullptr);
    ASSERT_NE(member, nullptr);
    const auto sources = MakeSources(owner, 0, member, 0);
    scene::Transform transform;
    scene::WaterZoneSnapshot zone;
    std::string diagnostic;
    ASSERT_TRUE(scene::BuildWaterZoneSnapshot(sources, {}, transform, 10.0f, 1000.0f,
        zone, diagnostic)) << diagnostic;
    EXPECT_NEAR(SurfaceArea(zone.geometry), 20.0, 1.0e-5);
    EXPECT_TRUE(ContainsPoint(zone.geometry, 1.0f, 2.0f));
    EXPECT_TRUE(ContainsPoint(zone.geometry, 5.0f, 2.0f));
    EXPECT_FALSE(ContainsPoint(zone.geometry, 6.1f, 2.0f));
    EXPECT_FALSE(zone.boundaryEdges.empty());
    for (const auto& edge : zone.boundaryEdges) {
        const auto& a = zone.geometry.vertices[edge[0]].worldPosition;
        const auto& b = zone.geometry.vertices[edge[1]].worldPosition;
        const bool interiorSeam = std::abs(a.x - 4.0f) < 1.0e-5f
            && std::abs(b.x - 4.0f) < 1.0e-5f
            && (std::min)(a.z, b.z) >= 1.0f - 1.0e-5f
            && (std::max)(a.z, b.z) <= 3.0f + 1.0e-5f;
        EXPECT_FALSE(interiorSeam);
    }
}

TEST_F(WaterZoneTest, PreservesHolesAndDisconnectedBodies)
{
    std::vector<scene::WaterZoneSource> sources{
        {MakeRectangle(0.0f, 4.0f, 0.0f, 1.0f, 0.0f), 0},
        {MakeRectangle(0.0f, 4.0f, 3.0f, 4.0f, 0.0f), 0},
        {MakeRectangle(0.0f, 1.0f, 1.0f, 3.0f, 0.0f), 0},
        {MakeRectangle(3.0f, 4.0f, 1.0f, 3.0f, 0.0f), 0},
        {MakeRectangle(6.0f, 8.0f, 0.0f, 2.0f, 0.0f), 0},
    };
    for (const auto& source : sources) ASSERT_NE(source.geometry, nullptr);
    scene::Transform transform;
    scene::WaterZoneSnapshot zone;
    std::string diagnostic;
    ASSERT_TRUE(scene::BuildWaterZoneSnapshot(sources, {}, transform, 10.0f, 1000.0f,
        zone, diagnostic)) << diagnostic;
    EXPECT_NEAR(SurfaceArea(zone.geometry), 16.0, 1.0e-5);
    EXPECT_FALSE(ContainsPoint(zone.geometry, 2.0f, 2.0f));
    EXPECT_TRUE(ContainsPoint(zone.geometry, 0.5f, 2.0f));
    EXPECT_TRUE(ContainsPoint(zone.geometry, 7.0f, 1.0f));
    EXPECT_FALSE(ContainsPoint(zone.geometry, 5.0f, 1.0f));
}

TEST_F(WaterZoneTest, HigherPriorityContainedSourcesWinRegardlessOfWhichIsTheOwner)
{
    scene::Transform transform;
    std::string diagnostic;
    scene::WaterZoneSnapshot zone;
    auto owner = MakeRectangle(-5.0f, 5.0f, -5.0f, 5.0f, 0.0f);
    auto member = MakeRectangle(-1.0f, 1.0f, -1.0f, 1.0f, 5.0f);
    ASSERT_NE(owner, nullptr);
    ASSERT_NE(member, nullptr);
    auto sources = MakeSources(owner, 0, member, 10);
    ASSERT_TRUE(scene::BuildWaterZoneSnapshot(sources, {}, transform, 10.0f, 1000.0f,
        zone, diagnostic)) << diagnostic;
    scene::WaterSplineSample sample;
    ASSERT_TRUE(scene::TrySampleWaterSpline(zone.geometry, 0.0f, 0.0f, sample));
    EXPECT_NEAR(sample.baseHeightOffset, 5.0f, testkit::kTolerance);

    owner = MakeRectangle(-1.0f, 1.0f, -1.0f, 1.0f, 5.0f);
    member = MakeRectangle(-5.0f, 5.0f, -5.0f, 5.0f, 0.0f);
    ASSERT_NE(owner, nullptr);
    ASSERT_NE(member, nullptr);
    sources = MakeSources(owner, 10, member, 0);
    ASSERT_TRUE(scene::BuildWaterZoneSnapshot(sources, {}, transform, 10.0f, 1000.0f,
        zone, diagnostic)) << diagnostic;
    ASSERT_TRUE(scene::TrySampleWaterSpline(zone.geometry, 0.0f, 0.0f, sample));
    EXPECT_NEAR(sample.baseHeightOffset, 5.0f, testkit::kTolerance);
}

TEST_F(WaterZoneTest, RiverMouthBlendsAtItsEndpointAndBlendLengthAtDefaultPriority)
{
    const auto owner = MakeRectangle(-5.0f, 5.0f, -5.0f, 5.0f, 0.0f,
        0.0f, 2.0f, math::Vector3::ZERO);
    ASSERT_NE(owner, nullptr);
    std::array<scene::WaterSplinePoint, 3> riverPoints{};
    riverPoints[0] = {1, {5.0f, 0.0f, 0.0f}, 2.0f, 4.0f, 6.0f};
    riverPoints[1] = {2, {10.0f, 0.0f, 0.0f}, 2.0f, 4.0f, 6.0f};
    riverPoints[2] = {3, {15.0f, 0.0f, 0.0f}, 2.0f, 4.0f, 6.0f};
    scene::Transform riverTransform;
    riverTransform.worldPosition.y = 8.0f;
    scene::WaterSplineSnapshot riverSnapshot;
    ASSERT_TRUE(scene::BuildWaterSplineSnapshot(scene::WaterSurfaceShape::SPLINE_RIVER,
        riverPoints, 2, riverTransform, riverSnapshot, 10.0f, 1000.0f));
    auto river = std::make_shared<const scene::WaterSplineSnapshot>(std::move(riverSnapshot));
    const std::array<scene::WaterZoneSource, 2> sources{{{owner, 0}, {river, 0}}};
    const std::array<scene::WaterZoneMouthInput, 1> mouths{{
        {1, {5.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, 4.0f},
    }};
    scene::Transform ownerTransform;
    scene::WaterZoneSnapshot zone;
    std::string diagnostic;
    ASSERT_TRUE(scene::BuildWaterZoneSnapshot(sources, mouths, ownerTransform,
        10.0f, 1.0f, zone, diagnostic, true)) << diagnostic;
    ASSERT_EQ(zone.materials.size(), zone.geometry.vertices.size());
    scene::WaterSplineSample sample;
    ASSERT_TRUE(scene::TrySampleWaterSpline(zone.geometry, 5.0f, 0.0f, sample));
    EXPECT_NEAR(sample.baseHeightOffset, 0.0f, testkit::kTolerance);
    EXPECT_NEAR(sample.depth, 2.0f, testkit::kTolerance);
    const scene::WaterSplineVertex* interiorVertex = nullptr;
    double closestDistance = std::numeric_limits<double>::infinity();
    for (const scene::WaterSplineVertex& vertex : zone.geometry.vertices) {
        const double distance = static_cast<double>(vertex.worldPosition.x) - 5.0;
        if (distance <= 0.01 || distance >= 3.99) continue;
        const double difference = std::abs(distance - 2.0);
        if (difference < closestDistance) {
            closestDistance = difference;
            interiorVertex = &vertex;
        }
    }
    ASSERT_NE(interiorVertex, nullptr);
    ASSERT_TRUE(scene::TrySampleWaterSpline(zone.geometry, interiorVertex->worldPosition.x,
        interiorVertex->worldPosition.z, sample));
    const double normalizedDistance = (interiorVertex->worldPosition.x - 5.0) / 4.0;
    const double blendWeight = normalizedDistance * normalizedDistance
        * (3.0 - 2.0 * normalizedDistance);
    const auto& material = zone.materials[static_cast<std::size_t>(interiorVertex - zone.geometry.vertices.data())];
    EXPECT_EQ(material.receiver, 0u);
    EXPECT_EQ(material.source, 1u);
    EXPECT_NEAR(material.sourceWeight, blendWeight, testkit::kTolerance);
    EXPECT_NEAR(sample.baseHeightOffset, 8.0 * blendWeight, 0.05f);
    EXPECT_NEAR(sample.depth, 2.0 + 2.0 * blendWeight, 0.05f);
    EXPECT_NEAR(sample.worldVelocity.x, 6.0 * blendWeight, 0.05f);
    ASSERT_TRUE(scene::TrySampleWaterSpline(zone.geometry, 9.0f, 0.0f, sample));
    EXPECT_NEAR(sample.baseHeightOffset, 8.0f, testkit::kTolerance);
    EXPECT_NEAR(sample.depth, 4.0f, testkit::kTolerance);
    EXPECT_NEAR(sample.worldVelocity.x, 6.0f, 0.05f);
}

TEST_F(WaterZoneTest, TributaryUsesTheSpecifiedReceiverInTheSameZone)
{
    const auto owner = MakeRectangle(-8.0f, -6.0f, -2.0f, 2.0f, 0.0f,
        0.0f, 2.0f, math::Vector3::ZERO);
    const auto receiver = MakeRectangle(-5.0f, 5.0f, -5.0f, 5.0f, 0.0f,
        3.0f, 2.0f, math::Vector3{2.0f, 0.0f, 0.0f});
    ASSERT_NE(owner, nullptr);
    ASSERT_NE(receiver, nullptr);
    const std::array<scene::WaterSplinePoint, 2> points{{
        {1, {5.0f, 8.0f, 0.0f}, 2.0f, 4.0f, 6.0f},
        {2, {10.0f, 8.0f, 0.0f}, 2.0f, 4.0f, 6.0f},
    }};
    scene::Transform transform;
    scene::WaterSplineSnapshot spline;
    ASSERT_TRUE(scene::BuildWaterSplineSnapshot(scene::WaterSurfaceShape::SPLINE_RIVER,
        points, 2, transform, spline, 10.0f, 1000.0f));
    const auto tributary = std::make_shared<const scene::WaterSplineSnapshot>(std::move(spline));
    const std::array<scene::WaterZoneSource, 3> sources{{{owner, 0}, {receiver, 0}, {tributary, 0}}};
    const std::array<scene::WaterZoneMouthInput, 1> mouths{{
        {2, {5.0f, 8.0f, 0.0f}, {1.0f, 0.0f}, 4.0f, 1},
    }};
    scene::WaterZoneSnapshot zone;
    std::string diagnostic;
    ASSERT_TRUE(scene::BuildWaterZoneSnapshot(sources, mouths, transform, 10.0f, 1.0f, zone, diagnostic)) << diagnostic;
    scene::WaterSplineSample sample;
    ASSERT_TRUE(scene::TrySampleWaterSpline(zone.geometry, 5.0f, 0.0f, sample));
    EXPECT_NEAR(sample.baseHeightOffset, 3.0f, testkit::kTolerance);
    EXPECT_NEAR(sample.worldVelocity.x, 2.0f, testkit::kTolerance);
    ASSERT_TRUE(scene::TrySampleWaterSpline(zone.geometry, 9.0f, 0.0f, sample));
    EXPECT_NEAR(sample.baseHeightOffset, 8.0f, testkit::kTolerance);
    EXPECT_NEAR(sample.worldVelocity.x, 6.0f, testkit::kTolerance);
}

TEST_F(WaterZoneTest, RelativeSurfaceOffsetsSurviveLargeWorldHeightAndTranslatedSmallGeometry)
{
    constexpr float baseY = 100000000.0f;
    auto owner = MakeRectangle(0.0f, 10.0f, 0.0f, 10.0f, baseY);
    auto member = MakeRectangle(2.0f, 8.0f, 2.0f, 8.0f, baseY, 1.0f);
    ASSERT_NE(owner, nullptr);
    ASSERT_NE(member, nullptr);
    const auto sources = MakeSources(owner, 0, member, 5);
    scene::Transform ownerTransform;
    ownerTransform.worldPosition.y = baseY;
    scene::WaterZoneSnapshot tallZone;
    std::string diagnostic;
    ASSERT_TRUE(scene::BuildWaterZoneSnapshot(sources, {}, ownerTransform, 10.0f,
        1000.0f, tallZone, diagnostic)) << diagnostic;
    scene::WaterSplineSample sample;
    ASSERT_TRUE(scene::TrySampleWaterSpline(tallZone.geometry, 5.0f, 5.0f, sample));
    EXPECT_NEAR(sample.baseHeightOffset, 1.0f, testkit::kTolerance);

    const auto originOwner = MakeRectangle(0.0f, 1.0f, 0.0f, 1.0f, 0.0f);
    const auto originMember = MakeRectangle(1.0f, 2.0f, 0.0f, 1.0f, 0.0f);
    const auto shiftedOwner = MakeRectangle(1000000.0f, 1000001.0f,
        1000000.0f, 1000001.0f, 0.0f);
    const auto shiftedMember = MakeRectangle(1000001.0f, 1000002.0f,
        1000000.0f, 1000001.0f, 0.0f);
    ASSERT_NE(originOwner, nullptr);
    ASSERT_NE(originMember, nullptr);
    ASSERT_NE(shiftedOwner, nullptr);
    ASSERT_NE(shiftedMember, nullptr);
    scene::WaterZoneSnapshot originZone;
    scene::WaterZoneSnapshot shiftedZone;
    const auto originSources = MakeSources(originOwner, 0, originMember, 0);
    const auto shiftedSources = MakeSources(shiftedOwner, 0, shiftedMember, 0);
    ASSERT_TRUE(scene::BuildWaterZoneSnapshot(originSources, {}, scene::Transform{},
        10.0f, 1000.0f, originZone, diagnostic)) << diagnostic;
    ASSERT_TRUE(scene::BuildWaterZoneSnapshot(shiftedSources, {}, scene::Transform{},
        10.0f, 1000.0f, shiftedZone, diagnostic)) << diagnostic;
    EXPECT_NEAR(SurfaceArea(originZone.geometry), 2.0, 1.0e-5);
    EXPECT_NEAR(SurfaceArea(shiftedZone.geometry), SurfaceArea(originZone.geometry), 1.0e-5);

    const auto broadSource = MakeRectangle(0.0f, 0.1f, 0.0f, 0.1f, 0.0f);
    const auto tinySource = MakeRectangle(0.2f, 0.20001f, 0.0f, 0.00001f, 0.0f);
    ASSERT_NE(broadSource, nullptr);
    ASSERT_NE(tinySource, nullptr);
    const auto tinySources = MakeSources(broadSource, 0, tinySource, 1);
    scene::WaterZoneSnapshot tinyZone;
    ASSERT_TRUE(scene::BuildWaterZoneSnapshot(tinySources, {}, scene::Transform{},
        10.0f, 1000.0f, tinyZone, diagnostic)) << diagnostic;
    EXPECT_NEAR(SurfaceArea(tinyZone.geometry),
        SurfaceArea(*broadSource) + SurfaceArea(*tinySource), 2.0e-12);
    ASSERT_TRUE(scene::TrySampleWaterSpline(tinyZone.geometry, 0.200005f, 0.000005f, sample));
}

TEST_F(WaterZoneTest, SupportsSeparateRiverMouthsAtBothEndpoints)
{
    const auto owner = MakeRectangle(-6.0f, 6.0f, -5.0f, 5.0f, 0.0f);
    ASSERT_NE(owner, nullptr);
    std::array<scene::WaterSplinePoint, 3> riverPoints{};
    riverPoints[0] = {1, {-5.0f, 0.0f, 0.0f}, 2.0f, 2.0f, 3.0f};
    riverPoints[1] = {2, {0.0f, 0.0f, 0.0f}, 2.0f, 2.0f, 3.0f};
    riverPoints[2] = {3, {5.0f, 0.0f, 0.0f}, 2.0f, 2.0f, 3.0f};
    scene::Transform riverTransform;
    riverTransform.worldPosition.y = 4.0f;
    scene::WaterSplineSnapshot riverSnapshot;
    ASSERT_TRUE(scene::BuildWaterSplineSnapshot(scene::WaterSurfaceShape::SPLINE_RIVER,
        riverPoints, 2, riverTransform, riverSnapshot, 10.0f, 1000.0f));
    const auto river = std::make_shared<const scene::WaterSplineSnapshot>(std::move(riverSnapshot));
    const std::array<scene::WaterZoneSource, 2> sources{{{owner, 0}, {river, 0}}};
    const std::array<scene::WaterZoneMouthInput, 2> mouths{{
        {1, {-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, 2.0f},
        {1, {5.0f, 0.0f, 0.0f}, {-1.0f, 0.0f}, 2.0f},
    }};
    scene::WaterZoneSnapshot zone;
    std::string diagnostic;
    testkit::RecordingLogSink logSink;
    testkit::ScopedLogSink scopedLogSink(&logSink);
    const bool built = scene::BuildWaterZoneSnapshot(sources, mouths, scene::Transform{},
        10.0f, 1000.0f, zone, diagnostic);
    std::string failureDetails = diagnostic;
    for (const core::LogEntry& entry : logSink.entries) {
        if (entry.message.find("WaterZone topology:") == std::string::npos) continue;
        failureDetails += "\n";
        failureDetails += entry.message;
    }
    ASSERT_TRUE(built) << failureDetails;
    scene::WaterSplineSample sample;
    ASSERT_TRUE(scene::TrySampleWaterSpline(zone.geometry, -5.0f, 0.0f, sample));
    EXPECT_NEAR(sample.baseHeightOffset, 0.0f, testkit::kTolerance);
    ASSERT_TRUE(scene::TrySampleWaterSpline(zone.geometry, -3.0f, 0.0f, sample));
    EXPECT_NEAR(sample.baseHeightOffset, 4.0f, 0.05f);
    ASSERT_TRUE(scene::TrySampleWaterSpline(zone.geometry, 3.0f, 0.0f, sample));
    EXPECT_NEAR(sample.baseHeightOffset, 4.0f, 0.05f);
    ASSERT_TRUE(scene::TrySampleWaterSpline(zone.geometry, 5.0f, 0.0f, sample));
    EXPECT_NEAR(sample.baseHeightOffset, 0.0f, testkit::kTolerance);
    ASSERT_TRUE(scene::TrySampleWaterSpline(zone.geometry, 0.0f, 0.0f, sample));
    EXPECT_NEAR(sample.baseHeightOffset, 0.0f, testkit::kTolerance);
}

TEST_F(WaterZoneTest, PreservesFloatRepresentableThinMemberAdjacentToOwner)
{
    const auto owner = MakeRectangle(0.0f, 10.0f, -1.0f, 0.0f, 0.0f);
    const std::array<math::Vector2, 3> thinPoints{{
        {0.0f, 0.0f}, {10.0f, 4.0e-13f}, {5.0f, 0.0f},
    }};
    const auto thinMember = MakeTriangle(thinPoints);
    ASSERT_NE(owner, nullptr);
    ASSERT_NE(thinMember, nullptr);
    const std::array<scene::WaterZoneSource, 2> sources{{{owner, 1}, {thinMember, 0}}};
    scene::WaterZoneSnapshot zone;
    std::string diagnostic;
    ASSERT_TRUE(scene::BuildWaterZoneSnapshot(sources, {}, scene::Transform{},
        10.0f, 1000.0f, zone, diagnostic)) << diagnostic;
    EXPECT_NEAR(SurfaceArea(zone.geometry), 10.0 + 1.0e-12, 1.0e-13);
}

TEST_F(WaterZoneTest, CanonicalizesEquivalentClippingIntersectionsForTransformedMembers)
{
    struct Case {
        float yawDegrees;
        float originX;
        float originZ;
        math::Vector3 memberScale;
    };
    const std::array<Case, 3> cases{{
        {90.0f, 0.0f, 0.0f, {2.0f, 1.0f, 0.5f}},
        {270.0f, 128.0f, -256.0f, {2.0f, 1.0f, 0.5f}},
        {90.0f, -512.0f, 256.0f, {-2.0f, 1.0f, 0.5f}},
    }};
    std::string diagnostic;
    for (const Case& testCase : cases) {
        const auto owner = MakeRectangle(testCase.originX - 4.0f, testCase.originX + 4.0f,
            testCase.originZ - 4.0f, testCase.originZ + 4.0f, 0.0f);
        const auto member = MakeTransformedRectangle(
            {testCase.originX + 4.5f, 0.0f, testCase.originZ},
            testCase.yawDegrees, testCase.memberScale, 4.0f);
        ASSERT_NE(owner, nullptr);
        ASSERT_NE(member, nullptr);
        const std::array<scene::WaterZoneSource, 2> sources{{{owner, 0}, {member, 1}}};
        scene::Transform ownerTransform;
        ownerTransform.worldPosition = {testCase.originX, 0.0f, testCase.originZ};
        scene::WaterZoneSnapshot zone;
        ASSERT_TRUE(scene::BuildWaterZoneSnapshot(sources, {}, ownerTransform,
            10.0f, 1000.0f, zone, diagnostic)) << diagnostic;
        ASSERT_FALSE(zone.boundaryEdges.empty());
        EXPECT_NEAR(SurfaceArea(zone.geometry), 116.0, 0.02);
    }
}

TEST_F(WaterZoneTest, RejectsUnsupportedOwnerRotationWithoutChangingOutput)
{
    const auto owner = MakeRectangle(-2.0f, 2.0f, -2.0f, 2.0f, 0.0f);
    ASSERT_NE(owner, nullptr);
    const std::array<scene::WaterZoneSource, 1> sources{{{owner, 0}}};
    scene::Transform invalidTransform;
    invalidTransform.worldRotation = {0.0f, 0.0f, 0.0f, 0.0f};
    scene::WaterZoneSnapshot out;
    out.geometry.signature = 91;
    out.geometry.vertices.push_back({});
    std::string diagnostic;
    EXPECT_FALSE(scene::BuildWaterZoneSnapshot(sources, {}, invalidTransform,
        10.0f, 1.0f, out, diagnostic));
    EXPECT_FALSE(diagnostic.empty());
    EXPECT_EQ(out.geometry.signature, 91u);
    EXPECT_EQ(out.geometry.vertices.size(), 1u);
}

TEST_F(WaterZoneTest, AcceptsDryWaterAndRejectsAmbiguousTranslatedObliqueIntersectionsAtomically)
{
    const auto dryOwner = MakeRectangle(0.0f, 4.0f, 0.0f, 4.0f, 0.0f, 0.0f, 0.0f);
    const auto dryMember = MakeRectangle(3.0f, 6.0f, 0.0f, 4.0f, 0.0f, 0.0f, 0.0f);
    ASSERT_NE(dryOwner, nullptr);
    ASSERT_NE(dryMember, nullptr);
    const auto drySources = MakeSources(dryOwner, 0, dryMember, 0);
    scene::WaterZoneSnapshot dryZone;
    std::string diagnostic;
    ASSERT_TRUE(scene::BuildWaterZoneSnapshot(drySources, {}, scene::Transform{},
        0.0f, 1000.0f, dryZone, diagnostic)) << diagnostic;
    scene::WaterSplineSample sample;
    ASSERT_TRUE(scene::TrySampleWaterSpline(dryZone.geometry, 3.5f, 2.0f, sample));
    EXPECT_FLOAT_EQ(sample.depth, 0.0f);

    constexpr float origin = 1000000.0f;
    const auto owner = MakeRectangle(origin, origin + 3.0f, origin, origin + 3.0f, 0.0f);
    const float cosine = std::cos(0.61f);
    const float sine = std::sin(0.61f);
    const std::array<math::Vector2, 4> rotatedPoints{{
        {origin + 2.0f - 1.8f * cosine + 1.1f * sine,
            origin + 1.5f - 1.8f * sine - 1.1f * cosine},
        {origin + 2.0f + 1.8f * cosine + 1.1f * sine,
            origin + 1.5f + 1.8f * sine - 1.1f * cosine},
        {origin + 2.0f + 1.8f * cosine - 1.1f * sine,
            origin + 1.5f + 1.8f * sine + 1.1f * cosine},
        {origin + 2.0f - 1.8f * cosine - 1.1f * sine,
            origin + 1.5f - 1.8f * sine + 1.1f * cosine},
    }};
    const auto member = MakeQuad(rotatedPoints);
    ASSERT_NE(owner, nullptr);
    ASSERT_NE(member, nullptr);
    const auto sources = MakeSources(owner, 0, member, 1);
    scene::WaterZoneSnapshot obliqueZone;
    obliqueZone.geometry.signature = 77;
    obliqueZone.geometry.vertices.push_back({});
    const bool built = scene::BuildWaterZoneSnapshot(sources, {}, scene::Transform{},
        10.0f, 1000.0f, obliqueZone, diagnostic);
    if (!built) {
        EXPECT_FALSE(diagnostic.empty());
        EXPECT_EQ(obliqueZone.geometry.signature, 77u);
        EXPECT_EQ(obliqueZone.geometry.vertices.size(), 1u);
        return;
    }
    ASSERT_FALSE(obliqueZone.boundaryEdges.empty());
    for (const auto& edge : obliqueZone.boundaryEdges) {
        const auto& a = obliqueZone.geometry.vertices[edge[0]].worldPosition;
        const auto& b = obliqueZone.geometry.vertices[edge[1]].worldPosition;
        const double dx = static_cast<double>(b.x) - a.x;
        const double dz = static_cast<double>(b.z) - a.z;
        const double length = std::hypot(dx, dz);
        ASSERT_GT(length, 0.0);
        const float midpointX = (a.x + b.x) * 0.5f;
        const float midpointZ = (a.z + b.z) * 0.5f;
        const float offsetX = static_cast<float>(-dz / length * 0.25);
        const float offsetZ = static_cast<float>(dx / length * 0.25);
        const bool sideA = UnionContains(sources, midpointX + offsetX, midpointZ + offsetZ);
        const bool sideB = UnionContains(sources, midpointX - offsetX, midpointZ - offsetZ);
        EXPECT_FALSE(sideA && sideB);
    }
}

} /// @note namespace fbzz::tests
