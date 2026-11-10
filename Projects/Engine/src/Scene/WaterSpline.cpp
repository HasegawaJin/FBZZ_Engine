/// @file    WaterSpline.cpp
/// @brief   Build and sample the shared numeric spline surface for water.
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <Engine/Scene/WaterSpline.hpp>
#include <Engine/Scene/WaterSplineMesh.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/WaterZoneSnapshot.hpp>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace fbzz::scene {

/// @brief Clip the lake footprint using the same bounded tessellation as water zones.
bool SubtractLakeIslands(WaterSplineSnapshot& lake, std::span<const WaterSplineSnapshot> islands,
    const Transform& transform);

namespace {
constexpr std::uint32_t MINIMUM_SAMPLES_PER_SEGMENT = 1;
constexpr std::uint32_t MAXIMUM_SAMPLES_PER_SEGMENT = WATER_SPLINE_MAX_SAMPLES_PER_SEGMENT;
constexpr std::size_t MAXIMUM_CONTROL_POINTS = WATER_SPLINE_MAX_CONTROL_POINTS;
constexpr std::size_t MAXIMUM_SNAPSHOT_VERTICES = WATER_SPLINE_MAX_VERTICES;
constexpr std::uint64_t HASH_OFFSET = 14695981039346656037ULL;
constexpr std::uint64_t HASH_PRIME = 1099511628211ULL;
constexpr std::size_t BVH_LEAF_TRIANGLE_COUNT = 8;

struct Point2 {
    double x = 0.0;
    double z = 0.0;
};

struct SplineValue {
    math::Vector3 position = math::Vector3::ZERO;
    math::Vector3 tangent = math::Vector3::ZERO;
    float width = 0.0f;
    float depth = 0.0f;
    float flowSpeed = 0.0f;
};

bool Finite(const math::Vector2& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool SupportedTransform(const Transform& transform)
{
    const auto scale = transform.worldScale;
    const double rotationLength = transform.worldRotation.Length();
    const auto up = transform.worldRotation * math::Vector3::UP;
    return Finite(transform.worldPosition) && Finite(scale)
        && std::abs(scale.x) >= 0.0001f && std::abs(scale.y) >= 0.0001f && std::abs(scale.z) >= 0.0001f
        && std::isfinite(rotationLength) && std::abs(rotationLength - 1.0) <= 0.001
        && Finite(up) && std::abs(up.x) <= 0.00001f && std::abs(up.z) <= 0.00001f
        && std::abs(std::abs(up.y) - 1.0f) <= 0.00001f;
}

std::uint64_t HashByte(std::uint64_t hash, std::uint8_t value)
{
    return (hash ^ value) * HASH_PRIME;
}

std::uint64_t HashU32(std::uint64_t hash, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
        hash = HashByte(hash, static_cast<std::uint8_t>(value >> shift));
    return hash;
}

std::uint64_t HashFloat(std::uint64_t hash, float value)
{
    return HashU32(hash, std::bit_cast<std::uint32_t>(value));
}

double Cross(const Point2& a, const Point2& b, const Point2& c)
{
    return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

bool OnSegment(const Point2& a, const Point2& b, const Point2& p, double epsilon)
{
    return std::abs(Cross(a, b, p)) <= epsilon
        && p.x >= (std::min)(a.x, b.x) - epsilon && p.x <= (std::max)(a.x, b.x) + epsilon
        && p.z >= (std::min)(a.z, b.z) - epsilon && p.z <= (std::max)(a.z, b.z) + epsilon;
}

bool SegmentsIntersect(const Point2& a, const Point2& b, const Point2& c, const Point2& d, double epsilon)
{
    const double abC = Cross(a, b, c);
    const double abD = Cross(a, b, d);
    const double cdA = Cross(c, d, a);
    const double cdB = Cross(c, d, b);
    if (((abC > epsilon && abD < -epsilon) || (abC < -epsilon && abD > epsilon))
        && ((cdA > epsilon && cdB < -epsilon) || (cdA < -epsilon && cdB > epsilon))) return true;
    return OnSegment(a, b, c, epsilon) || OnSegment(a, b, d, epsilon)
        || OnSegment(c, d, a, epsilon) || OnSegment(c, d, b, epsilon);
}

bool IsSimplePolygon(const std::vector<Point2>& polygon, double epsilon)
{
    const std::size_t count = polygon.size();
    if (count < 3) return false;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t nextI = (i + 1) % count;
        const double dx = polygon[nextI].x - polygon[i].x;
        const double dz = polygon[nextI].z - polygon[i].z;
        if (dx * dx + dz * dz <= epsilon * epsilon) return false;
        for (std::size_t j = i + 1; j < count; ++j) {
            const std::size_t nextJ = (j + 1) % count;
            if (i == j || nextI == j || nextJ == i) continue;
            if (SegmentsIntersect(polygon[i], polygon[nextI], polygon[j], polygon[nextJ], epsilon)) return false;
        }
    }
    return true;
}

bool PointInTriangle(const Point2& p, const Point2& a, const Point2& b, const Point2& c,
    double orientation, double epsilon)
{
    return orientation * Cross(a, b, p) >= -epsilon
        && orientation * Cross(b, c, p) >= -epsilon
        && orientation * Cross(c, a, p) >= -epsilon;
}

/// @brief Triangulate a simple polygon by removing ears in input order.
/// @see https://www.geometrictools.com/Documentation/TriangulationByEarClipping.pdf David Eberly, Triangulation by Ear Clipping.
bool TriangulatePolygon(const std::vector<Point2>& polygon, double epsilon,
    std::vector<std::array<std::uint32_t, 3>>& triangles)
{
    double signedArea = 0.0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Point2& a = polygon[i];
        const Point2& b = polygon[(i + 1) % polygon.size()];
        signedArea += a.x * b.z - b.x * a.z;
    }
    if (!std::isfinite(signedArea) || std::abs(signedArea) <= epsilon) return false;
    const double orientation = signedArea > 0.0 ? 1.0 : -1.0;
    std::vector<std::uint32_t> remaining;
    remaining.reserve(polygon.size());
    for (std::size_t i = 0; i < polygon.size(); ++i)
        remaining.push_back(static_cast<std::uint32_t>(i));
    triangles.clear();
    triangles.reserve(polygon.size() - 2);
    std::size_t attempts = 0;
    const std::size_t maximumAttempts = polygon.size() * polygon.size();
    while (remaining.size() > 3 && attempts++ < maximumAttempts) {
        bool clipped = false;
        for (std::size_t i = 0; i < remaining.size(); ++i) {
            const std::uint32_t previous = remaining[(i + remaining.size() - 1) % remaining.size()];
            const std::uint32_t current = remaining[i];
            const std::uint32_t next = remaining[(i + 1) % remaining.size()];
            const Point2& a = polygon[previous];
            const Point2& b = polygon[current];
            const Point2& c = polygon[next];
            if (orientation * Cross(a, b, c) <= epsilon) continue;
            bool containsPoint = false;
            for (const std::uint32_t candidate : remaining) {
                if (candidate == previous || candidate == current || candidate == next) continue;
                if (PointInTriangle(polygon[candidate], a, b, c, orientation, epsilon)) {
                    containsPoint = true;
                    break;
                }
            }
            if (containsPoint) continue;
            triangles.push_back({previous, current, next});
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) return false;
    }
    if (remaining.size() != 3) return false;
    if (orientation * Cross(polygon[remaining[0]], polygon[remaining[1]], polygon[remaining[2]]) <= epsilon)
        return false;
    triangles.push_back({remaining[0], remaining[1], remaining[2]});
    return true;
}

/// @brief Evaluate one uniformly parameterized Catmull-Rom segment and linearly interpolated physical metadata.
/// @see https://doi.org/10.1145/54852.378511 Barry and Goldman, SIGGRAPH 1988: recursive evaluation of Catmull-Rom splines.
SplineValue EvaluateSpline(const std::span<const WaterSplinePoint> points, std::size_t segment,
    float t, bool closed)
{
    const std::size_t count = points.size();
    const auto pointAt = [&](std::ptrdiff_t index) -> const WaterSplinePoint& {
        if (closed) {
            const auto n = static_cast<std::ptrdiff_t>(count);
            index = (index % n + n) % n;
        } else {
            index = (std::clamp)(index, std::ptrdiff_t{0}, static_cast<std::ptrdiff_t>(count - 1));
        }
        return points[static_cast<std::size_t>(index)];
    };
    const auto i = static_cast<std::ptrdiff_t>(segment);
    const auto& p0 = pointAt(i - 1);
    const auto& p1 = pointAt(i);
    const auto& p2 = pointAt(i + 1);
    const auto& p3 = pointAt(i + 2);
    const float t2 = t * t;
    const float t3 = t2 * t;
    const auto catmull = [t, t2, t3](float a, float b, float c, float d) {
        return 0.5f * (2.0f * b + (-a + c) * t + (2.0f * a - 5.0f * b + 4.0f * c - d) * t2
            + (-a + 3.0f * b - 3.0f * c + d) * t3);
    };
    const auto tangent = [t, t2](float a, float b, float c, float d) {
        return 0.5f * ((-a + c) + 2.0f * (2.0f * a - 5.0f * b + 4.0f * c - d) * t
            + 3.0f * (-a + 3.0f * b - 3.0f * c + d) * t2);
    };
    const auto linear = [t](float a, float b) { return a + (b - a) * t; };
    SplineValue value;
    value.position = {
        catmull(p0.localPosition.x, p1.localPosition.x, p2.localPosition.x, p3.localPosition.x),
        catmull(p0.localPosition.y, p1.localPosition.y, p2.localPosition.y, p3.localPosition.y),
        catmull(p0.localPosition.z, p1.localPosition.z, p2.localPosition.z, p3.localPosition.z),
    };
    value.tangent = {
        tangent(p0.localPosition.x, p1.localPosition.x, p2.localPosition.x, p3.localPosition.x),
        tangent(p0.localPosition.y, p1.localPosition.y, p2.localPosition.y, p3.localPosition.y),
        tangent(p0.localPosition.z, p1.localPosition.z, p2.localPosition.z, p3.localPosition.z),
    };
    value.width = linear(p1.width, p2.width);
    value.depth = linear(p1.depth, p2.depth);
    value.flowSpeed = linear(p1.flowSpeed, p2.flowSpeed);
    return value;
}

math::Vector3 TransformLocalDelta(const math::Vector3& local, const Transform& transform)
{
    const math::Vector3 scaled{local.x * transform.worldScale.x,
        local.y * transform.worldScale.y, local.z * transform.worldScale.z};
    return transform.worldRotation * scaled;
}

bool MakeWorldVelocity(const math::Vector3& tangent, float speed,
    const Transform& transform, math::Vector3& velocity)
{
    const math::Vector3 scaledTangent{tangent.x * transform.worldScale.x,
        0.0f, tangent.z * transform.worldScale.z};
    const math::Vector3 worldTangent = transform.worldRotation * scaledTangent;
    const double length = std::hypot(static_cast<double>(worldTangent.x), static_cast<double>(worldTangent.z));
    if (!std::isfinite(length) || length <= 1.0e-8 || !std::isfinite(speed)) return false;
    velocity = {static_cast<float>(worldTangent.x / length * speed), 0.0f,
        static_cast<float>(worldTangent.z / length * speed)};
    return Finite(velocity);
}

double PolygonEpsilon(const std::vector<Point2>& polygon)
{
    double minX = std::numeric_limits<double>::infinity();
    double minZ = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double maxZ = -std::numeric_limits<double>::infinity();
    for (const Point2& p : polygon) {
        minX = (std::min)(minX, p.x);
        minZ = (std::min)(minZ, p.z);
        maxX = (std::max)(maxX, p.x);
        maxZ = (std::max)(maxZ, p.z);
    }
    const double scale = (std::max)({maxX - minX, maxZ - minZ, 1.0});
    return scale * scale * 1.0e-10;
}

bool ComputeTriangleGradient(const std::vector<WaterSplineVertex>& vertices,
    const std::array<std::uint32_t, 3>& indices, math::Vector2& gradient)
{
    const auto& p0 = vertices[indices[0]].worldPosition;
    const auto& p1 = vertices[indices[1]].worldPosition;
    const auto& p2 = vertices[indices[2]].worldPosition;
    const double dx1 = static_cast<double>(p1.x) - p0.x;
    const double dz1 = static_cast<double>(p1.z) - p0.z;
    const double dx2 = static_cast<double>(p2.x) - p0.x;
    const double dz2 = static_cast<double>(p2.z) - p0.z;
    const double determinant = dx1 * dz2 - dx2 * dz1;
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12) return false;
    const double dy1 = static_cast<double>(vertices[indices[1]].baseHeightOffset)
        - vertices[indices[0]].baseHeightOffset;
    const double dy2 = static_cast<double>(vertices[indices[2]].baseHeightOffset)
        - vertices[indices[0]].baseHeightOffset;
    const double gradientX = (dy1 * dz2 - dy2 * dz1) / determinant;
    const double gradientZ = (dx1 * dy2 - dx2 * dy1) / determinant;
    if (!std::isfinite(gradientX) || !std::isfinite(gradientZ)
        || std::abs(gradientX) > std::numeric_limits<float>::max()
        || std::abs(gradientZ) > std::numeric_limits<float>::max()) return false;
    gradient = {static_cast<float>(gradientX), static_cast<float>(gradientZ)};
    return true;
}

bool AddTriangle(WaterSplineSnapshot& snapshot, std::uint32_t a, std::uint32_t b, std::uint32_t c)
{
    const std::array<std::uint32_t, 3> indices{a, b, c};
    math::Vector2 gradient;
    if (!ComputeTriangleGradient(snapshot.vertices, indices, gradient)) return false;
    snapshot.triangles.push_back({indices, gradient});
    return true;
}

bool ValidateAuthoring(WaterSurfaceShape shape, std::span<const WaterSplinePoint> points,
    std::uint32_t samplesPerSegment, float maximumWaterDepth, float maximumWorldCellSize)
{
    const bool lake = shape == WaterSurfaceShape::SPLINE_LAKE;
    if (!lake && shape != WaterSurfaceShape::SPLINE_RIVER) return false;
    if (points.size() < (lake ? 3u : 2u) || points.size() > MAXIMUM_CONTROL_POINTS
        || samplesPerSegment < MINIMUM_SAMPLES_PER_SEGMENT || samplesPerSegment > MAXIMUM_SAMPLES_PER_SEGMENT
        || !std::isfinite(maximumWaterDepth) || maximumWaterDepth < 0.0f
        || !std::isfinite(maximumWorldCellSize) || maximumWorldCellSize <= 0.0f)
        return false;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto& point = points[i];
        if (point.id == 0 || point.id > WATER_SPLINE_MAX_POINT_ID
            || !Finite(point.localPosition) || !std::isfinite(point.width)
            || !std::isfinite(point.depth) || point.depth < 0.0f || !std::isfinite(point.flowSpeed)) return false;
        if (shape == WaterSurfaceShape::SPLINE_RIVER && point.width <= 0.0f) return false;
        for (std::size_t j = 0; j < i; ++j)
            if (points[j].id == point.id) return false;
        if (lake && std::abs(point.localPosition.y - points[0].localPosition.y) > 0.0001f) return false;
    }
    return true;
}

void IncludeBounds(WaterSplineSnapshot& snapshot, const WaterSplineVertex& vertex, bool first)
{
    const auto include = [first](math::Vector3 value, math::Vector3& minimum, math::Vector3& maximum) {
        if (first) {
            minimum = maximum = value;
            return;
        }
        minimum.x = (std::min)(minimum.x, value.x);
        minimum.y = (std::min)(minimum.y, value.y);
        minimum.z = (std::min)(minimum.z, value.z);
        maximum.x = (std::max)(maximum.x, value.x);
        maximum.y = (std::max)(maximum.y, value.y);
        maximum.z = (std::max)(maximum.z, value.z);
    };
    include(vertex.localPosition, snapshot.localBoundsMin, snapshot.localBoundsMax);
    include(vertex.worldPosition, snapshot.worldBoundsMin, snapshot.worldBoundsMax);
    if (first) snapshot.minimumBaseHeightOffset = snapshot.maximumBaseHeightOffset = vertex.baseHeightOffset;
    else {
        snapshot.minimumBaseHeightOffset = (std::min)(snapshot.minimumBaseHeightOffset, vertex.baseHeightOffset);
        snapshot.maximumBaseHeightOffset = (std::max)(snapshot.maximumBaseHeightOffset, vertex.baseHeightOffset);
    }
    snapshot.maximumDepth = (std::max)(snapshot.maximumDepth, vertex.depth);
}

bool AddVertex(WaterSplineSnapshot& snapshot, const SplineValue& value,
    const math::Vector3& localPosition, const Transform& transform,
    float maximumWaterDepth, std::uint32_t& index)
{
    if (snapshot.vertices.size() >= MAXIMUM_SNAPSHOT_VERTICES) return false;
    const math::Vector3 worldDelta = TransformLocalDelta(localPosition, transform);
    const math::Vector3 worldPosition = transform.worldPosition + worldDelta;
    math::Vector3 worldVelocity;
    if (!MakeWorldVelocity(value.tangent, value.flowSpeed, transform, worldVelocity)) return false;
    const float depth = (std::min)(value.depth, maximumWaterDepth);
    const float baseHeightOffset = worldDelta.y;
    if (!Finite(worldPosition) || !std::isfinite(depth) || depth < 0.0f
        || !std::isfinite(baseHeightOffset)) return false;
    WaterSplineVertex vertex{localPosition, worldPosition, worldVelocity, baseHeightOffset, depth};
    IncludeBounds(snapshot, vertex, snapshot.vertices.empty());
    snapshot.vertices.push_back(vertex);
    index = static_cast<std::uint32_t>(snapshot.vertices.size() - 1);
    return true;
}

bool BuildLake(WaterSplineSnapshot& snapshot, std::span<const WaterSplinePoint> points,
    std::uint32_t samplesPerSegment, const Transform& transform, float maximumWaterDepth)
{
    const std::size_t count = points.size() * samplesPerSegment;
    if (count > WATER_SPLINE_MAX_LAKE_BOUNDARY_VERTICES) return false;
    std::vector<Point2> polygon;
    polygon.reserve(count);
    snapshot.outlineIndices.reserve(count);
    for (std::size_t segment = 0; segment < points.size(); ++segment) {
        for (std::uint32_t sample = 0; sample < samplesPerSegment; ++sample) {
            const float t = static_cast<float>(sample) / static_cast<float>(samplesPerSegment);
            const SplineValue value = EvaluateSpline(points, segment, t, true);
            math::Vector3 local{value.position.x, points[0].localPosition.y, value.position.z};
            std::uint32_t index = 0;
            if (!AddVertex(snapshot, value, local, transform, maximumWaterDepth, index)) return false;
            const auto& world = snapshot.vertices[index].worldPosition;
            polygon.push_back({world.x, world.z});
            snapshot.outlineIndices.push_back(index);
        }
    }
    const double epsilon = PolygonEpsilon(polygon);
    if (!IsSimplePolygon(polygon, std::sqrt(epsilon))) return false;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    if (!TriangulatePolygon(polygon, epsilon, triangles)) return false;
    for (const auto& triangle : triangles)
        if (!AddTriangle(snapshot, triangle[0], triangle[1], triangle[2])) return false;
    return true;
}

bool LakeOutlinesIntersect(const WaterSplineSnapshot& a, const WaterSplineSnapshot& b)
{
    const auto worldPoint = [](const auto& mesh, std::size_t index) {
        const auto& point = mesh.vertices[mesh.outlineIndices[index]].worldPosition;
        return Point2{point.x, point.z};
    };
    for (std::size_t i = 0; i < a.outlineIndices.size(); ++i) {
        const auto start = worldPoint(a, i);
        const auto end = worldPoint(a, (i + 1) % a.outlineIndices.size());
        for (std::size_t j = 0; j < b.outlineIndices.size(); ++j)
            if (SegmentsIntersect(start, end, worldPoint(b, j),
                    worldPoint(b, (j + 1) % b.outlineIndices.size()), 1.0e-9)) return true;
    }
    return false;
}

/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/water-body-actors-in-unreal-engine Epic Water Body Actors: Water Body Island.
bool ApplyLakeIslands(WaterSplineSnapshot& lake, std::span<const WaterLakeIslandPoint> islands,
    const Transform& transform, float localHeight)
{
    if (islands.size() > WATER_LAKE_MAX_ISLAND_POINTS || !RebuildWaterSplineSnapshotDerivedData(lake))
        return false;
    std::array<std::vector<WaterSplinePoint>, WATER_LAKE_MAX_ISLANDS> rings;
    for (const auto& point : islands) {
        if (point.ring < 0 || point.ring >= WATER_LAKE_MAX_ISLANDS || !Finite(point.localPosition)) return false;
        auto& ring = rings[point.ring];
        ring.push_back({static_cast<std::uint32_t>(ring.size() + 1),
            {point.localPosition.x, localHeight, point.localPosition.z}});
    }
    std::vector<WaterSplineSnapshot> cutouts;
    const auto contains = [](const WaterSplineSnapshot& mesh, const math::Vector3& point) {
        WaterSplineSample sample;
        return TrySampleWaterSpline(mesh, point.x, point.z, sample);
    };
    for (const auto& ring : rings) {
        if (ring.empty()) continue;
        if (ring.size() < 3 || ring.size() > WATER_SPLINE_MAX_CONTROL_POINTS) return false;
        WaterSplineSnapshot cutout;
        cutout.worldBaseY = lake.worldBaseY;
        if (!BuildLake(cutout, ring, 1, transform, 0.0f)
            || !RebuildWaterSplineSnapshotDerivedData(cutout)
            || !contains(lake, cutout.vertices.front().worldPosition) || LakeOutlinesIntersect(lake, cutout)) return false;
        for (const auto& other : cutouts)
            if (LakeOutlinesIntersect(other, cutout) || contains(other, cutout.vertices.front().worldPosition)
                || contains(cutout, other.vertices.front().worldPosition)) return false;
        auto& outline = lake.islandOutlines.emplace_back();
        for (const auto index : cutout.outlineIndices)
            outline.push_back(cutout.vertices[index].worldPosition);
        cutouts.push_back(std::move(cutout));
    }
    return SubtractLakeIslands(lake, cutouts, transform);
}

bool IsConvexQuad(const Point2& a, const Point2& b, const Point2& c, const Point2& d, double epsilon)
{
    const std::array<double, 4> cross{Cross(a, b, c), Cross(b, c, d), Cross(c, d, a), Cross(d, a, b)};
    bool positive = false;
    bool negative = false;
    for (const double value : cross) {
        if (value > epsilon) positive = true;
        if (value < -epsilon) negative = true;
    }
    return !(positive && negative) && (positive || negative);
}

bool BuildRiver(WaterSplineSnapshot& snapshot, std::span<const WaterSplinePoint> points,
    std::uint32_t samplesPerSegment, const Transform& transform, float maximumWaterDepth)
{
    const std::size_t segmentCount = points.size() - 1;
    const std::size_t sampleCount = segmentCount * samplesPerSegment + 1;
    if (sampleCount * 2 > WATER_SPLINE_MAX_RIVER_BOUNDARY_VERTICES) return false;
    std::vector<std::uint32_t> left;
    std::vector<std::uint32_t> right;
    left.reserve(sampleCount);
    right.reserve(sampleCount);
    for (std::size_t segment = 0; segment < segmentCount; ++segment) {
        for (std::uint32_t sample = 0; sample < samplesPerSegment; ++sample) {
            const float t = static_cast<float>(sample) / static_cast<float>(samplesPerSegment);
            const SplineValue value = EvaluateSpline(points, segment, t, false);
            const double tangentLength = std::hypot(static_cast<double>(value.tangent.x),
                static_cast<double>(value.tangent.z));
            if (!std::isfinite(tangentLength) || tangentLength <= 1.0e-8
                || !std::isfinite(value.width) || value.width <= 0.0f) return false;
            const float normalX = static_cast<float>(-value.tangent.z / tangentLength);
            const float normalZ = static_cast<float>(value.tangent.x / tangentLength);
            const float halfWidth = value.width * 0.5f;
            const math::Vector3 leftLocal{value.position.x + normalX * halfWidth,
                value.position.y, value.position.z + normalZ * halfWidth};
            const math::Vector3 rightLocal{value.position.x - normalX * halfWidth,
                value.position.y, value.position.z - normalZ * halfWidth};
            std::uint32_t leftIndex = 0;
            std::uint32_t rightIndex = 0;
            if (!AddVertex(snapshot, value, leftLocal, transform, maximumWaterDepth, leftIndex)
                || !AddVertex(snapshot, value, rightLocal, transform, maximumWaterDepth, rightIndex)) return false;
            left.push_back(leftIndex);
            right.push_back(rightIndex);
        }
    }
    const SplineValue end = EvaluateSpline(points, segmentCount - 1, 1.0f, false);
    const double endTangentLength = std::hypot(static_cast<double>(end.tangent.x),
        static_cast<double>(end.tangent.z));
    if (!std::isfinite(endTangentLength) || endTangentLength <= 1.0e-8
        || !std::isfinite(end.width) || end.width <= 0.0f) return false;
    const float endNormalX = static_cast<float>(-end.tangent.z / endTangentLength);
    const float endNormalZ = static_cast<float>(end.tangent.x / endTangentLength);
    const float endHalfWidth = end.width * 0.5f;
    std::uint32_t endLeft = 0;
    std::uint32_t endRight = 0;
    if (!AddVertex(snapshot, end, {end.position.x + endNormalX * endHalfWidth, end.position.y,
            end.position.z + endNormalZ * endHalfWidth}, transform, maximumWaterDepth, endLeft)
        || !AddVertex(snapshot, end, {end.position.x - endNormalX * endHalfWidth, end.position.y,
            end.position.z - endNormalZ * endHalfWidth}, transform, maximumWaterDepth, endRight)) return false;
    left.push_back(endLeft);
    right.push_back(endRight);

    std::vector<Point2> outline;
    outline.reserve(left.size() + right.size());
    snapshot.outlineIndices.reserve(left.size() + right.size());
    for (const std::uint32_t index : left) {
        const auto& p = snapshot.vertices[index].worldPosition;
        outline.push_back({p.x, p.z});
        snapshot.outlineIndices.push_back(index);
    }
    for (auto it = right.rbegin(); it != right.rend(); ++it) {
        const auto& p = snapshot.vertices[*it].worldPosition;
        outline.push_back({p.x, p.z});
        snapshot.outlineIndices.push_back(*it);
    }
    const double epsilon = PolygonEpsilon(outline);
    if (!IsSimplePolygon(outline, std::sqrt(epsilon))) return false;
    double stripOrientation = 0.0;
    for (std::size_t i = 0; i + 1 < left.size(); ++i) {
        const auto& leftA = snapshot.vertices[left[i]].worldPosition;
        const auto& rightA = snapshot.vertices[right[i]].worldPosition;
        const auto& leftB = snapshot.vertices[left[i + 1]].worldPosition;
        const auto& rightB = snapshot.vertices[right[i + 1]].worldPosition;
        const Point2 a{leftA.x, leftA.z}, b{rightA.x, rightA.z};
        const Point2 c{rightB.x, rightB.z}, d{leftB.x, leftB.z};
        if (!IsConvexQuad(a, b, c, d, epsilon)) return false;
        const double orientation = Cross(a, b, c);
        if (std::abs(orientation) <= epsilon) return false;
        if (stripOrientation == 0.0) stripOrientation = orientation;
        else if (orientation * stripOrientation <= 0.0) return false;
        if (!AddTriangle(snapshot, left[i], right[i], right[i + 1])
            || !AddTriangle(snapshot, left[i], right[i + 1], left[i + 1])) return false;
    }
    return !snapshot.triangles.empty();
}

math::Vector2 TriangleCentroid(const WaterSplineSnapshot& snapshot, std::uint32_t triangleIndex)
{
    const auto& triangle = snapshot.triangles[triangleIndex];
    const auto& a = snapshot.vertices[triangle.indices[0]].worldPosition;
    const auto& b = snapshot.vertices[triangle.indices[1]].worldPosition;
    const auto& c = snapshot.vertices[triangle.indices[2]].worldPosition;
    return {static_cast<float>((static_cast<double>(a.x) + b.x + c.x) / 3.0),
        static_cast<float>((static_cast<double>(a.z) + b.z + c.z) / 3.0)};
}

void BuildTriangleBvh(WaterSplineSnapshot& snapshot)
{
    snapshot.triangleOrder.resize(snapshot.triangles.size());
    std::iota(snapshot.triangleOrder.begin(), snapshot.triangleOrder.end(), 0u);
    snapshot.bvhNodes.reserve(snapshot.triangles.size() * 2);
    const auto build = [&](auto&& self, std::uint32_t first, std::uint32_t count) -> std::uint32_t {
        WaterSplineBvhNode node;
        node.boundsMin = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
        node.boundsMax = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
        math::Vector2 centroidMin = node.boundsMin;
        math::Vector2 centroidMax = node.boundsMax;
        for (std::uint32_t orderIndex = first; orderIndex < first + count; ++orderIndex) {
            const math::Vector2 center = TriangleCentroid(snapshot, snapshot.triangleOrder[orderIndex]);
            centroidMin.x = (std::min)(centroidMin.x, center.x);
            centroidMin.y = (std::min)(centroidMin.y, center.y);
            centroidMax.x = (std::max)(centroidMax.x, center.x);
            centroidMax.y = (std::max)(centroidMax.y, center.y);
            const auto& triangle = snapshot.triangles[snapshot.triangleOrder[orderIndex]];
            for (const std::uint32_t vertexIndex : triangle.indices) {
                const auto& position = snapshot.vertices[vertexIndex].worldPosition;
                node.boundsMin.x = (std::min)(node.boundsMin.x, position.x);
                node.boundsMin.y = (std::min)(node.boundsMin.y, position.z);
                node.boundsMax.x = (std::max)(node.boundsMax.x, position.x);
                node.boundsMax.y = (std::max)(node.boundsMax.y, position.z);
            }
        }
        const std::uint32_t nodeIndex = static_cast<std::uint32_t>(snapshot.bvhNodes.size());
        snapshot.bvhNodes.push_back(node);
        if (count <= BVH_LEAF_TRIANGLE_COUNT) {
            snapshot.bvhNodes[nodeIndex].first = first;
            snapshot.bvhNodes[nodeIndex].count = count;
            return nodeIndex;
        }
        const bool splitX = centroidMax.x - centroidMin.x >= centroidMax.y - centroidMin.y;
        const std::uint32_t middle = first + count / 2;
        std::nth_element(snapshot.triangleOrder.begin() + first, snapshot.triangleOrder.begin() + middle,
            snapshot.triangleOrder.begin() + first + count,
            [&](std::uint32_t a, std::uint32_t b) {
                const auto ca = TriangleCentroid(snapshot, a);
                const auto cb = TriangleCentroid(snapshot, b);
                const float coordinateA = splitX ? ca.x : ca.y;
                const float coordinateB = splitX ? cb.x : cb.y;
                return coordinateA < coordinateB || (coordinateA == coordinateB && a < b);
            });
        const std::uint32_t left = self(self, first, middle - first);
        const std::uint32_t right = self(self, middle, first + count - middle);
        snapshot.bvhNodes[nodeIndex].left = left;
        snapshot.bvhNodes[nodeIndex].right = right;
        return nodeIndex;
    };
    if (!snapshot.triangleOrder.empty())
        build(build, 0, static_cast<std::uint32_t>(snapshot.triangleOrder.size()));
}

} /// @note namespace

math::Vector2 WaterSurfaceLocalExtent(const WaterComponent& water, WaterSurfaceReadView view)
{
    if (!UsesWaterSurfaceGeometry(water, view)) return {water.extentX, water.extentZ};
    const WaterSplineSnapshot* geometry = WaterSurfaceGeometry(water, view);
    if (!geometry) return math::Vector2::ZERO;
    const auto& snapshot = *geometry;
    return {2.0f * (std::max)(std::abs(snapshot.localBoundsMin.x), std::abs(snapshot.localBoundsMax.x)),
        2.0f * (std::max)(std::abs(snapshot.localBoundsMin.z), std::abs(snapshot.localBoundsMax.z))};
}

WaterSurfaceShape WaterSurfaceEffectiveShape(const WaterComponent& water, WaterSurfaceReadView view)
{
    if (view == WaterSurfaceReadView::SIMULATION && water.simulationReadEnabled
        && water.simulationSurfacePublished)
        return water.simulationSurfaceShape;
    if (water.pointRuntimeMode == ControlPointRuntimeMode::BAKED_STATIC && water.bakeValid)
        return water.bakedSurfaceShape;
    return water.surfaceShape;
}

bool UsesWaterSurfaceGeometry(const WaterComponent& water, WaterSurfaceReadView view)
{
    if (view == WaterSurfaceReadView::SIMULATION && water.simulationReadEnabled
        && water.simulationSurfacePublished) {
        return water.simulationZoneSurfaceSelected || water.simulationZoneSnapshot
            || water.simulationSplineSnapshot
            || water.simulationSurfaceShape != WaterSurfaceShape::RECTANGLE;
    }
    return water.zoneSurfaceSelected || static_cast<bool>(water.zoneSnapshot)
        || WaterSurfaceEffectiveShape(water, view) != WaterSurfaceShape::RECTANGLE;
}

const WaterSplineSnapshot* WaterSurfaceGeometry(const WaterComponent& water, WaterSurfaceReadView view)
{
    if (view == WaterSurfaceReadView::SIMULATION && water.simulationReadEnabled
        && water.simulationSurfacePublished) {
        if (water.simulationZoneSnapshot) return &water.simulationZoneSnapshot->geometry;
        if (water.simulationZoneSurfaceSelected) return nullptr;
        if (water.simulationSplineSnapshot
            && water.simulationSplineSnapshot->shape == water.simulationSurfaceShape)
            return water.simulationSplineSnapshot.get();
        return nullptr;
    }
    if (water.zoneSurfaceSelected) {
        return water.zoneSnapshot ? &water.zoneSnapshot->geometry : nullptr;
    }
    if (water.zoneSnapshot) return &water.zoneSnapshot->geometry;
    const WaterSurfaceShape shape = WaterSurfaceEffectiveShape(water, view);
    if (shape == WaterSurfaceShape::RECTANGLE || !water.splineSnapshot
        || water.splineSnapshot->shape != shape) return nullptr;
    return water.splineSnapshot.get();
}

const Transform& WaterSurfaceTransform(const WaterComponent& water,
    const Transform& currentTransform, WaterSurfaceReadView view)
{
    return view == WaterSurfaceReadView::SIMULATION && water.simulationReadEnabled
        && water.simulationSurfacePublished
        ? water.simulationTransform : currentTransform;
}

EntityID WaterSurfaceZoneOwner(const WaterComponent& water, WaterSurfaceReadView view)
{
    return view == WaterSurfaceReadView::SIMULATION && water.simulationReadEnabled
        && water.simulationSurfacePublished
        ? water.simulationZoneOwner : water.zoneOwner;
}

bool HasValidWaterSplineExtents(const WaterSplineSnapshot& snapshot, const Transform& transform)
{
    constexpr double FLOAT_EXTENT_LIMIT = static_cast<double>(std::numeric_limits<float>::max())
        * (1.0 - 8.0 * std::numeric_limits<float>::epsilon());
    const double extentX = 2.0 * (std::max)(std::abs(static_cast<double>(snapshot.localBoundsMin.x)),
        std::abs(static_cast<double>(snapshot.localBoundsMax.x)));
    const double extentZ = 2.0 * (std::max)(std::abs(static_cast<double>(snapshot.localBoundsMin.z)),
        std::abs(static_cast<double>(snapshot.localBoundsMax.z)));
    if (!std::isfinite(extentX) || !std::isfinite(extentZ) || extentX <= 0.0 || extentZ <= 0.0
        || extentX > FLOAT_EXTENT_LIMIT || extentZ > FLOAT_EXTENT_LIMIT) return false;

    const double scaledExtentX = extentX * std::abs(static_cast<double>(transform.worldScale.x));
    const double scaledExtentZ = extentZ * std::abs(static_cast<double>(transform.worldScale.z));
    if (!std::isfinite(scaledExtentX) || !std::isfinite(scaledExtentZ)
        || scaledExtentX <= 0.0 || scaledExtentZ <= 0.0
        || scaledExtentX > FLOAT_EXTENT_LIMIT || scaledExtentZ > FLOAT_EXTENT_LIMIT) return false;

    const math::Vector3 worldAxisX = transform.worldRotation * math::Vector3{1.0f, 0.0f, 0.0f};
    const math::Vector3 worldAxisZ = transform.worldRotation * math::Vector3{0.0f, 0.0f, 1.0f};
    const double worldExtentX = std::abs(static_cast<double>(worldAxisX.x)) * scaledExtentX
        + std::abs(static_cast<double>(worldAxisZ.x)) * scaledExtentZ;
    const double worldExtentZ = std::abs(static_cast<double>(worldAxisX.z)) * scaledExtentX
        + std::abs(static_cast<double>(worldAxisZ.z)) * scaledExtentZ;
    return std::isfinite(worldExtentX) && std::isfinite(worldExtentZ)
        && worldExtentX > 0.0 && worldExtentZ > 0.0
        && worldExtentX <= FLOAT_EXTENT_LIMIT && worldExtentZ <= FLOAT_EXTENT_LIMIT;
}

std::uint64_t WaterSplineSnapshotSignature(WaterSurfaceShape shape,
    std::span<const WaterSplinePoint> points, std::uint32_t samplesPerSegment,
    const Transform& transform, float maximumWaterDepth, float maximumWorldCellSize,
    std::span<const WaterLakeIslandPoint> islands)
{
    std::uint64_t hash = HASH_OFFSET;
    hash = HashU32(hash, static_cast<std::uint32_t>(shape));
    hash = HashU32(hash, samplesPerSegment);
    hash = HashU32(hash, static_cast<std::uint32_t>(points.size()));
    hash = HashFloat(hash, maximumWaterDepth);
    hash = HashFloat(hash, maximumWorldCellSize);
    for (const auto& point : points) {
        hash = HashU32(hash, point.id);
        hash = HashFloat(hash, point.localPosition.x);
        hash = HashFloat(hash, point.localPosition.y);
        hash = HashFloat(hash, point.localPosition.z);
        hash = HashFloat(hash, point.width);
        hash = HashFloat(hash, point.depth);
        hash = HashFloat(hash, point.flowSpeed);
    }
    if (shape == WaterSurfaceShape::SPLINE_LAKE && !islands.empty()) {
        hash = HashU32(hash, static_cast<std::uint32_t>(islands.size()));
        for (const auto& point : islands) {
            hash = HashU32(hash, static_cast<std::uint32_t>(point.ring));
            hash = HashFloat(hash, point.localPosition.x);
            hash = HashFloat(hash, point.localPosition.y);
            hash = HashFloat(hash, point.localPosition.z);
        }
    }
    hash = HashFloat(hash, transform.worldPosition.x);
    hash = HashFloat(hash, transform.worldPosition.y);
    hash = HashFloat(hash, transform.worldPosition.z);
    hash = HashFloat(hash, transform.worldScale.x);
    hash = HashFloat(hash, transform.worldScale.y);
    hash = HashFloat(hash, transform.worldScale.z);
    hash = HashFloat(hash, transform.worldRotation.x);
    hash = HashFloat(hash, transform.worldRotation.y);
    hash = HashFloat(hash, transform.worldRotation.z);
    hash = HashFloat(hash, transform.worldRotation.w);
    return hash == 0 ? 1 : hash;
}

bool BuildWaterSplineSnapshot(WaterSurfaceShape shape, std::span<const WaterSplinePoint> points,
    std::uint32_t samplesPerSegment, const Transform& transform, WaterSplineSnapshot& out,
    float maximumWaterDepth, float maximumWorldCellSize, std::span<const WaterLakeIslandPoint> islands)
{
    if (!ValidateAuthoring(shape, points, samplesPerSegment, maximumWaterDepth, maximumWorldCellSize)
        || !SupportedTransform(transform)) return false;
    WaterSplineSnapshot snapshot;
    snapshot.shape = shape;
    snapshot.worldBaseY = transform.worldPosition.y;
    snapshot.signature = WaterSplineSnapshotSignature(shape, points, samplesPerSegment,
        transform, maximumWaterDepth, maximumWorldCellSize, islands);
    /// @note Point depth, width, and signed flow speed are linearly interpolated between controls so positive physical limits cannot spline-overshoot.
    /// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/water-body-actors-in-unreal-engine Epic Games, Water Body Actors: lake and river spline metadata.
    /// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/Components/USplineComponent Epic Games, USplineComponent spline evaluation.
    const bool success = shape == WaterSurfaceShape::SPLINE_LAKE
        ? BuildLake(snapshot, points, samplesPerSegment, transform, maximumWaterDepth)
        : BuildRiver(snapshot, points, samplesPerSegment, transform, maximumWaterDepth);
    if (!success || !HasValidWaterSplineExtents(snapshot, transform)) return false;
    if (shape == WaterSurfaceShape::SPLINE_LAKE && !islands.empty()
        && !ApplyLakeIslands(snapshot, islands, transform, points.front().localPosition.y)) return false;
    snapshot.footprintTriangles = snapshot.triangles;
    if (!FinalizeWaterSplineSnapshot(snapshot, maximumWorldCellSize)
        || snapshot.outlineIndices.size() < 3) return false;
    out = std::move(snapshot);
    return true;
}

bool FinalizeWaterSplineSnapshot(WaterSplineSnapshot& snapshot, float maximumWorldCellSize)
{
    if (!std::isfinite(maximumWorldCellSize) || maximumWorldCellSize <= 0.0f
        || snapshot.vertices.empty() || snapshot.vertices.size() > WATER_SPLINE_MAX_VERTICES
        || snapshot.triangles.empty() || snapshot.triangles.size() > 65536u
        || !std::isfinite(snapshot.worldBaseY)) return false;
    if (snapshot.footprintTriangles.empty()) snapshot.footprintTriangles = snapshot.triangles;
    if (snapshot.footprintTriangles.empty() || snapshot.footprintTriangles.size() > 65536u) return false;
    snapshot.localBoundsMin = snapshot.localBoundsMax = math::Vector3::ZERO;
    snapshot.worldBoundsMin = snapshot.worldBoundsMax = math::Vector3::ZERO;
    snapshot.minimumBaseHeightOffset = std::numeric_limits<float>::infinity();
    snapshot.maximumBaseHeightOffset = -std::numeric_limits<float>::infinity();
    snapshot.maximumDepth = 0.0f;
    for (std::size_t i = 0; i < snapshot.vertices.size(); ++i) {
        const auto& vertex = snapshot.vertices[i];
        if (!Finite(vertex.localPosition) || !Finite(vertex.worldPosition) || !Finite(vertex.worldVelocity)
            || !std::isfinite(vertex.baseHeightOffset) || !std::isfinite(vertex.depth) || vertex.depth < 0.0f)
            return false;
        IncludeBounds(snapshot, vertex, i == 0);
    }
    for (auto& triangle : snapshot.triangles) {
        if (triangle.indices[0] >= snapshot.vertices.size() || triangle.indices[1] >= snapshot.vertices.size()
            || triangle.indices[2] >= snapshot.vertices.size()
            || !ComputeTriangleGradient(snapshot.vertices, triangle.indices, triangle.baseHeightGradient)) return false;
    }
    for (const auto& triangle : snapshot.footprintTriangles)
        if (triangle.indices[0] >= snapshot.vertices.size() || triangle.indices[1] >= snapshot.vertices.size()
            || triangle.indices[2] >= snapshot.vertices.size()) return false;
    for (const auto index : snapshot.outlineIndices)
        if (index >= snapshot.vertices.size()) return false;

    RefineWaterSplineMesh(snapshot, maximumWorldCellSize);
    return RebuildWaterSplineSnapshotDerivedData(snapshot);
}

bool RebuildWaterSplineSnapshotDerivedData(WaterSplineSnapshot& snapshot)
{
    if (snapshot.vertices.empty() || snapshot.vertices.size() > WATER_SPLINE_MAX_VERTICES
        || snapshot.triangles.empty() || snapshot.triangles.size() > 65536u
        || !std::isfinite(snapshot.worldBaseY)) return false;
    snapshot.localBoundsMin = snapshot.localBoundsMax = math::Vector3::ZERO;
    snapshot.worldBoundsMin = snapshot.worldBoundsMax = math::Vector3::ZERO;
    snapshot.minimumBaseHeightOffset = std::numeric_limits<float>::infinity();
    snapshot.maximumBaseHeightOffset = -std::numeric_limits<float>::infinity();
    snapshot.maximumDepth = 0.0f;
    snapshot.maximumWorldEdgeLength = 0.0f;
    for (std::size_t i = 0; i < snapshot.vertices.size(); ++i) {
        const auto& vertex = snapshot.vertices[i];
        if (!Finite(vertex.localPosition) || !Finite(vertex.worldPosition) || !Finite(vertex.worldVelocity)
            || !std::isfinite(vertex.baseHeightOffset) || !std::isfinite(vertex.depth) || vertex.depth < 0.0f)
            return false;
        IncludeBounds(snapshot, vertex, i == 0);
    }
    for (auto& triangle : snapshot.triangles) {
        if (triangle.indices[0] >= snapshot.vertices.size() || triangle.indices[1] >= snapshot.vertices.size()
            || triangle.indices[2] >= snapshot.vertices.size()
            || !ComputeTriangleGradient(snapshot.vertices, triangle.indices, triangle.baseHeightGradient)) return false;
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const auto& a = snapshot.vertices[triangle.indices[edge]].worldPosition;
            const auto& b = snapshot.vertices[triangle.indices[(edge + 1) % 3]].worldPosition;
            const double dx = static_cast<double>(a.x) - b.x;
            const double dz = static_cast<double>(a.z) - b.z;
            snapshot.maximumWorldEdgeLength = (std::max)(snapshot.maximumWorldEdgeLength,
                static_cast<float>(std::hypot(dx, dz)));
        }
    }
    for (const auto& triangle : snapshot.footprintTriangles)
        if (triangle.indices[0] >= snapshot.vertices.size() || triangle.indices[1] >= snapshot.vertices.size()
            || triangle.indices[2] >= snapshot.vertices.size()) return false;
    for (const auto index : snapshot.outlineIndices)
        if (index >= snapshot.vertices.size()) return false;
    snapshot.bvhNodes.clear();
    snapshot.triangleOrder.clear();
    BuildTriangleBvh(snapshot);
    return !snapshot.triangles.empty() && !snapshot.bvhNodes.empty()
        && snapshot.triangleOrder.size() == snapshot.triangles.size()
        && Finite(snapshot.localBoundsMin) && Finite(snapshot.localBoundsMax)
        && Finite(snapshot.worldBoundsMin) && Finite(snapshot.worldBoundsMax)
        && std::isfinite(snapshot.minimumBaseHeightOffset) && std::isfinite(snapshot.maximumBaseHeightOffset)
        && std::isfinite(snapshot.maximumDepth) && snapshot.maximumDepth >= 0.0f
        && std::isfinite(snapshot.maximumWorldEdgeLength) && snapshot.maximumWorldEdgeLength > 0.0f;
}

bool TrySampleWaterSpline(const WaterSplineSnapshot& snapshot, float worldX, float worldZ,
    WaterSplineSample& out)
{
    if (!std::isfinite(worldX) || !std::isfinite(worldZ) || snapshot.vertices.empty()
        || snapshot.triangles.empty() || snapshot.bvhNodes.empty() || !std::isfinite(snapshot.worldBaseY)) return false;
    const float boundsScale = (std::max)({std::abs(snapshot.worldBoundsMin.x), std::abs(snapshot.worldBoundsMax.x),
        std::abs(snapshot.worldBoundsMin.z), std::abs(snapshot.worldBoundsMax.z), 1.0f});
    const float boundsTolerance = (std::max)(1.0e-5f, boundsScale * 8.0f * std::numeric_limits<float>::epsilon());
    if (worldX < snapshot.worldBoundsMin.x - boundsTolerance || worldX > snapshot.worldBoundsMax.x + boundsTolerance
        || worldZ < snapshot.worldBoundsMin.z - boundsTolerance || worldZ > snapshot.worldBoundsMax.z + boundsTolerance)
        return false;
    std::array<std::uint32_t, 64> stack{};
    std::size_t stackSize = 1;
    stack[0] = 0;
    while (stackSize > 0) {
        const auto nodeIndex = stack[--stackSize];
        if (nodeIndex >= snapshot.bvhNodes.size()) return false;
        const auto& node = snapshot.bvhNodes[nodeIndex];
        if (worldX < node.boundsMin.x - boundsTolerance || worldX > node.boundsMax.x + boundsTolerance
            || worldZ < node.boundsMin.y - boundsTolerance || worldZ > node.boundsMax.y + boundsTolerance) continue;
        if (node.count == 0) {
            if (stackSize + 2 > stack.size()) return false;
            stack[stackSize++] = node.left;
            stack[stackSize++] = node.right;
            continue;
        }
        if (static_cast<std::size_t>(node.first) + node.count > snapshot.triangleOrder.size()) return false;
        for (std::uint32_t candidate = node.first; candidate < node.first + node.count; ++candidate) {
            const std::uint32_t triangleIndex = snapshot.triangleOrder[candidate];
            if (triangleIndex >= snapshot.triangles.size()) return false;
            const auto& triangle = snapshot.triangles[triangleIndex];
            const std::uint32_t ia = triangle.indices[0];
            const std::uint32_t ib = triangle.indices[1];
            const std::uint32_t ic = triangle.indices[2];
            if (ia >= snapshot.vertices.size() || ib >= snapshot.vertices.size()
                || ic >= snapshot.vertices.size()) return false;
            const auto& a = snapshot.vertices[ia].worldPosition;
            const auto& b = snapshot.vertices[ib].worldPosition;
            const auto& c = snapshot.vertices[ic].worldPosition;
            const double denominator = (static_cast<double>(b.z) - c.z)
                    * (static_cast<double>(a.x) - c.x)
                + (static_cast<double>(c.x) - b.x) * (static_cast<double>(a.z) - c.z);
            if (!std::isfinite(denominator) || std::abs(denominator) <= 1.0e-12) return false;
            const double queryX = static_cast<double>(worldX);
            const double queryZ = static_cast<double>(worldZ);
            const double wa = ((static_cast<double>(b.z) - c.z) * (queryX - c.x)
                + (static_cast<double>(c.x) - b.x) * (queryZ - c.z)) / denominator;
            const double wb = ((static_cast<double>(c.z) - a.z) * (queryX - c.x)
                + (static_cast<double>(a.x) - c.x) * (queryZ - c.z)) / denominator;
            const double wc = 1.0 - wa - wb;
            constexpr double BARYCENTRIC_TOLERANCE = 1.0e-7;
            if (wa < -BARYCENTRIC_TOLERANCE || wb < -BARYCENTRIC_TOLERANCE
                || wc < -BARYCENTRIC_TOLERANCE) continue;
            const auto& va = snapshot.vertices[ia];
            const auto& vb = snapshot.vertices[ib];
            const auto& vc = snapshot.vertices[ic];
            const double baseHeightOffset = wa * va.baseHeightOffset + wb * vb.baseHeightOffset
                + wc * vc.baseHeightOffset;
            const double depth = wa * va.depth + wb * vb.depth + wc * vc.depth;
            const double flowX = wa * va.worldVelocity.x + wb * vb.worldVelocity.x + wc * vc.worldVelocity.x;
            const double flowZ = wa * va.worldVelocity.z + wb * vb.worldVelocity.z + wc * vc.worldVelocity.z;
            WaterSplineSample sample;
            sample.baseHeightOffset = static_cast<float>(baseHeightOffset);
            sample.baseHeight = snapshot.worldBaseY + sample.baseHeightOffset;
            sample.depth = static_cast<float>(depth);
            sample.worldVelocity = {static_cast<float>(flowX), 0.0f, static_cast<float>(flowZ)};
            sample.baseHeightGradient = triangle.baseHeightGradient;
            if (!std::isfinite(sample.baseHeight) || !std::isfinite(sample.baseHeightOffset)
                || !std::isfinite(sample.depth) || sample.depth < 0.0f
                || !Finite(sample.worldVelocity) || !Finite(sample.baseHeightGradient)) return false;
            out = sample;
            return true;
        }
    }
    return false;
}
} /// @note namespace fbzz::scene
