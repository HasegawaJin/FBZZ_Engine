/// @file    WaterZoneSnapshot.cpp
/// @brief   Build the bounded exact triangle union and river-mouth attributes for a Water Zone.
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <Engine/Scene/WaterZoneSnapshot.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/WaterSplineMesh.hpp>
#include <Engine/Core/Logger.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <utility>

namespace fbzz::scene {
namespace {
constexpr std::size_t MAX_ZONE_SOURCES = 32;
constexpr std::size_t MAX_ZONE_MOUTHS = 32;
constexpr std::size_t MAX_ZONE_TRIANGLES = 65536;
constexpr std::size_t MAX_ZONE_VERTICES = WATER_SPLINE_MAX_VERTICES;
constexpr std::size_t MAX_ZONE_WORK = 4'000'000;

struct Point2 {
    double x = 0.0;
    double z = 0.0;
    bool generatedIntersection = false;
};

struct Bounds2 {
    double minX = std::numeric_limits<double>::infinity();
    double minZ = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double maxZ = -std::numeric_limits<double>::infinity();
};

struct Triangle2 {
    std::array<Point2, 3> points{};
};

struct Polygon2 {
    std::vector<Point2> points;
};

struct MouthGeometry {
    WaterZoneMouthInput input;
    WaterSplineSample receiverEndpoint;
    std::vector<Triangle2> corridor;
};

struct TriangleNode {
    Bounds2 bounds;
    std::uint32_t left = 0;
    std::uint32_t right = 0;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

bool Finite(const math::Vector2& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

double Cross(const Point2& a, const Point2& b, const Point2& c)
{
    return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

double SignedArea(const std::vector<Point2>& points)
{
    if (points.size() < 3) return 0.0;
    const Point2& origin = points.front();
    double area = 0.0;
    for (std::size_t i = 1; i + 1 < points.size(); ++i)
        area += Cross(origin, points[i], points[i + 1]);
    return area * 0.5;
}

/// @brief Returns a coordinate's conservative radial float-rounding bound.
double FloatRoundingRadius(const Point2& point)
{
    const auto halfStep = [](double coordinate) {
        const float value = static_cast<float>(coordinate);
        const float nextUp = std::nextafter(value, std::numeric_limits<float>::infinity());
        const float nextDown = std::nextafter(value, -std::numeric_limits<float>::infinity());
        const double up = std::isfinite(nextUp) ? std::abs(static_cast<double>(nextUp) - value) : 0.0;
        const double down = std::isfinite(nextDown) ? std::abs(static_cast<double>(value) - nextDown) : 0.0;
        return (std::max)(up, down) * 0.5;
    };
    return std::hypot(halfStep(point.x), halfStep(point.z));
}

/// @brief Checks whether two numeric points are indistinguishable in the emitted float mesh.
/// @note Callers also require at least one point to be a generated clipping intersection.
bool FloatRoundoffEquivalent(const Point2& a, const Point2& b)
{
    if (static_cast<float>(a.x) != static_cast<float>(b.x)
        || static_cast<float>(a.z) != static_cast<float>(b.z)) return false;
    return std::hypot(a.x - b.x, a.z - b.z)
        <= FloatRoundingRadius(a) + FloatRoundingRadius(b);
}

Bounds2 Bounds(const Triangle2& triangle)
{
    Bounds2 bounds;
    for (const Point2& point : triangle.points) {
        bounds.minX = (std::min)(bounds.minX, point.x);
        bounds.minZ = (std::min)(bounds.minZ, point.z);
        bounds.maxX = (std::max)(bounds.maxX, point.x);
        bounds.maxZ = (std::max)(bounds.maxZ, point.z);
    }
    return bounds;
}

Bounds2 Bounds(const Polygon2& polygon)
{
    Bounds2 bounds;
    for (const Point2& point : polygon.points) {
        bounds.minX = (std::min)(bounds.minX, point.x);
        bounds.minZ = (std::min)(bounds.minZ, point.z);
        bounds.maxX = (std::max)(bounds.maxX, point.x);
        bounds.maxZ = (std::max)(bounds.maxZ, point.z);
    }
    return bounds;
}

bool Intersects(const Bounds2& a, const Bounds2& b, double epsilon)
{
    return a.minX <= b.maxX + epsilon && a.maxX + epsilon >= b.minX
        && a.minZ <= b.maxZ + epsilon && a.maxZ + epsilon >= b.minZ;
}

bool NormalizeTriangle(Triangle2& triangle, double areaEpsilon)
{
    const double area = Cross(triangle.points[0], triangle.points[1], triangle.points[2]);
    if (!std::isfinite(area) || std::abs(area) <= areaEpsilon) return false;
    if (area < 0.0) std::swap(triangle.points[1], triangle.points[2]);
    return true;
}

/// @brief Tests whether a point lies strictly inside a segment within orientation roundoff.
/// @note The determinant bound scales with its product terms and does not use the geometric merge tolerance.
/// @see Jonathan Richard Shewchuk, “Adaptive Precision Floating-Point Arithmetic and Fast Robust Predicates for Computational Geometry,” Robust Orientation Tests: https://www.cs.cmu.edu/~quake/robust.html
bool PointWithinSegmentRoundoff(const Point2& a, const Point2& b, const Point2& point)
{
    const double dx = b.x - a.x;
    const double dz = b.z - a.z;
    const double lengthSquared = dx * dx + dz * dz;
    if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0) return false;
    const double projection = ((point.x - a.x) * dx + (point.z - a.z) * dz) / lengthSquared;
    if (!std::isfinite(projection) || projection <= 0.0 || projection >= 1.0) return false;
    const double left = dx * (point.z - a.z);
    const double right = dz * (point.x - a.x);
    const double cross = left - right;
    const double roundoffBound = 8.0 * std::numeric_limits<double>::epsilon()
        * (std::abs(left) + std::abs(right));
    return std::abs(cross) <= roundoffBound;
}

/// @brief Removes duplicate and orientation-roundoff-collinear vertices from a clipped polygon.
/// @note The geometric merge tolerance applies only to duplicates; collinear cleanup requires a strictly intermediate point within the determinant roundoff bound.
void CleanPolygon(Polygon2& polygon, double epsilon)
{
    if (polygon.points.size() < 2) return;
    std::vector<Point2> clean;
    clean.reserve(polygon.points.size());
    for (const Point2& point : polygon.points) {
        if (clean.empty()) {
            clean.push_back(point);
            continue;
        }
        const Point2& previous = clean.back();
        if (std::hypot(point.x - previous.x, point.z - previous.z) > epsilon)
            clean.push_back(point);
    }
    if (clean.size() > 1) {
        const Point2& first = clean.front();
        const Point2& last = clean.back();
        if (std::hypot(first.x - last.x, first.z - last.z) <= epsilon) clean.pop_back();
    }
    bool removedCollinear = true;
    while (removedCollinear && clean.size() >= 3) {
        removedCollinear = false;
        for (std::size_t i = 0; i < clean.size(); ++i) {
            const Point2& previous = clean[(i + clean.size() - 1) % clean.size()];
            const Point2& current = clean[i];
            const Point2& next = clean[(i + 1) % clean.size()];
            if (!PointWithinSegmentRoundoff(previous, next, current)) continue;
            clean.erase(clean.begin() + static_cast<std::ptrdiff_t>(i));
            removedCollinear = true;
            break;
        }
    }
    polygon.points = std::move(clean);
}

bool ClipHalfPlane(const Polygon2& input, const Point2& a, const Point2& b,
    double orientation, bool keepInside, double epsilon, std::size_t& work,
    Polygon2& output)
{
    output.points.clear();
    if (input.points.empty()) return true;
    output.points.reserve(input.points.size() + 1);
    const auto signedDistance = [&](const Point2& point) { return orientation * Cross(a, b, point); };
    for (std::size_t i = 0; i < input.points.size(); ++i) {
        if (++work > MAX_ZONE_WORK) return false;
        const Point2& current = input.points[i];
        const Point2& next = input.points[(i + 1) % input.points.size()];
        const double currentDistance = signedDistance(current);
        const double nextDistance = signedDistance(next);
        const bool currentInside = currentDistance >= 0.0;
        const bool nextInside = nextDistance >= 0.0;
        const bool currentKept = currentInside == keepInside;
        const bool nextKept = nextInside == keepInside;
        if (currentKept) output.points.push_back(current);
        if (currentInside != nextInside) {
            const double denominator = currentDistance - nextDistance;
            if (std::isfinite(denominator) && std::abs(denominator) > 1.0e-20) {
                const double t = (std::clamp)(currentDistance / denominator, 0.0, 1.0);
                output.points.push_back({current.x + (next.x - current.x) * t,
                    current.z + (next.z - current.z) * t, true});
            }
        }
        if (!currentKept && nextKept && nextInside == currentInside) output.points.push_back(next);
    }
    CleanPolygon(output, epsilon);
    return true;
}

bool SubtractTriangle(const Polygon2& subject, const Triangle2& clip, double epsilon,
    std::size_t& work, std::vector<Polygon2>& remainder, Polygon2* intersection = nullptr)
{
    if (intersection) intersection->points.clear();
    Polygon2 inside = subject;
    Polygon2 outside;
    std::vector<Polygon2> pieces;
    pieces.reserve(3);
    for (std::size_t edge = 0; edge < 3 && inside.points.size() >= 3; ++edge) {
        Polygon2 nextInside;
        if (!ClipHalfPlane(inside, clip.points[edge], clip.points[(edge + 1) % 3],
                1.0, false, epsilon, work, outside)
            || !ClipHalfPlane(inside, clip.points[edge], clip.points[(edge + 1) % 3],
                1.0, true, epsilon, work, nextInside)) return false;
        if (outside.points.size() >= 3) {
            const double area = SignedArea(outside.points);
            if (!std::isfinite(area)) return false;
            if (area != 0.0) pieces.push_back(std::move(outside));
        }
        outside = {};
        inside = std::move(nextInside);
    }
    if (intersection && inside.points.size() >= 3) {
        const double area = SignedArea(inside.points);
        if (!std::isfinite(area)) return false;
        if (area != 0.0) *intersection = std::move(inside);
    }
    remainder.insert(remainder.end(), std::make_move_iterator(pieces.begin()),
        std::make_move_iterator(pieces.end()));
    if (remainder.size() > MAX_ZONE_TRIANGLES) return false;
    return true;
}

bool SubtractByIndex(std::vector<Polygon2>& fragments, const std::vector<Triangle2>& clips,
    const class TriangleIndex& index, double epsilon, std::size_t& work);
bool SplitByIndex(std::vector<Polygon2>& fragments, const std::vector<Triangle2>& clips,
    const class TriangleIndex& index, double epsilon, std::size_t& work);

class TriangleIndex {
public:
    bool Build(std::span<const Triangle2> triangles)
    {
        m_triangles.assign(triangles.begin(), triangles.end());
        m_order.resize(m_triangles.size());
        std::iota(m_order.begin(), m_order.end(), 0u);
        m_nodes.clear();
        m_nodes.reserve(m_triangles.size() * 2);
        if (!m_order.empty()) BuildNode(0, static_cast<std::uint32_t>(m_order.size()));
        return m_nodes.size() <= MAX_ZONE_TRIANGLES * 2;
    }

    template<typename F>
    bool Query(const Bounds2& bounds, double epsilon, F&& callback, std::size_t& work) const
    {
        if (m_nodes.empty()) return true;
        std::array<std::uint32_t, 64> stack{};
        std::size_t stackSize = 1;
        stack[0] = 0;
        while (stackSize > 0) {
            const std::uint32_t nodeIndex = stack[--stackSize];
            if (nodeIndex >= m_nodes.size()) return false;
            const TriangleNode& node = m_nodes[nodeIndex];
            if (!Intersects(bounds, node.bounds, epsilon)) continue;
            if (node.count == 0) {
                if (stackSize + 2 > stack.size()) return false;
                stack[stackSize++] = node.left;
                stack[stackSize++] = node.right;
                continue;
            }
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                if (++work > MAX_ZONE_WORK) return false;
                const std::uint32_t triangleIndex = m_order[i];
                if (Intersects(bounds, Bounds(m_triangles[triangleIndex]), epsilon)
                    && !callback(triangleIndex)) return false;
            }
        }
        return true;
    }

    [[nodiscard]] const std::vector<Triangle2>& Triangles() const { return m_triangles; }

private:
    std::uint32_t BuildNode(std::uint32_t first, std::uint32_t count)
    {
        TriangleNode node;
        Bounds2 centers;
        for (std::uint32_t i = first; i < first + count; ++i) {
            const Bounds2 bounds = Bounds(m_triangles[m_order[i]]);
            node.bounds.minX = (std::min)(node.bounds.minX, bounds.minX);
            node.bounds.minZ = (std::min)(node.bounds.minZ, bounds.minZ);
            node.bounds.maxX = (std::max)(node.bounds.maxX, bounds.maxX);
            node.bounds.maxZ = (std::max)(node.bounds.maxZ, bounds.maxZ);
            const double cx = (bounds.minX + bounds.maxX) * 0.5;
            const double cz = (bounds.minZ + bounds.maxZ) * 0.5;
            centers.minX = (std::min)(centers.minX, cx);
            centers.minZ = (std::min)(centers.minZ, cz);
            centers.maxX = (std::max)(centers.maxX, cx);
            centers.maxZ = (std::max)(centers.maxZ, cz);
        }
        const std::uint32_t nodeIndex = static_cast<std::uint32_t>(m_nodes.size());
        m_nodes.push_back(node);
        if (count <= 8) {
            m_nodes[nodeIndex].first = first;
            m_nodes[nodeIndex].count = count;
            return nodeIndex;
        }
        const bool splitX = centers.maxX - centers.minX >= centers.maxZ - centers.minZ;
        const std::uint32_t middle = first + count / 2;
        std::nth_element(m_order.begin() + first, m_order.begin() + middle, m_order.begin() + first + count,
            [&](std::uint32_t a, std::uint32_t b) {
                const Bounds2 boundsA = Bounds(m_triangles[a]);
                const Bounds2 boundsB = Bounds(m_triangles[b]);
                const double ca = splitX ? boundsA.minX + boundsA.maxX : boundsA.minZ + boundsA.maxZ;
                const double cb = splitX ? boundsB.minX + boundsB.maxX : boundsB.minZ + boundsB.maxZ;
                return ca < cb || (ca == cb && a < b);
            });
        const std::uint32_t left = BuildNode(first, middle - first);
        const std::uint32_t right = BuildNode(middle, first + count - middle);
        m_nodes[nodeIndex].left = left;
        m_nodes[nodeIndex].right = right;
        return nodeIndex;
    }

    std::vector<Triangle2> m_triangles;
    std::vector<std::uint32_t> m_order;
    std::vector<TriangleNode> m_nodes;
};

double PointSegmentDistance(const Point2& point, const Point2& a, const Point2& b)
{
    const double dx = b.x - a.x;
    const double dz = b.z - a.z;
    const double lengthSq = dx * dx + dz * dz;
    if (lengthSq <= 0.0) return std::hypot(point.x - a.x, point.z - a.z);
    const double t = (std::clamp)(((point.x - a.x) * dx + (point.z - a.z) * dz) / lengthSq,
        0.0, 1.0);
    return std::hypot(point.x - (a.x + dx * t), point.z - (a.z + dz * t));
}

bool TriangleCoversWithinRounding(const Point2& point, const Triangle2& triangle, double tolerance)
{
    bool inside = true;
    double nearest = std::numeric_limits<double>::infinity();
    for (std::size_t edge = 0; edge < 3; ++edge) {
        const Point2& a = triangle.points[edge];
        const Point2& b = triangle.points[(edge + 1) % 3];
        if (Cross(a, b, point) < 0.0) inside = false;
        nearest = (std::min)(nearest, PointSegmentDistance(point, a, b));
    }
    return inside || nearest <= tolerance;
}

bool SubtractByIndex(std::vector<Polygon2>& fragments, const std::vector<Triangle2>& clips,
    const TriangleIndex& index, double epsilon, std::size_t& work)
{
    std::vector<Polygon2> current;
    current.swap(fragments);
    for (const Polygon2& polygon : current) {
        std::vector<std::uint32_t> candidates;
        if (!index.Query(Bounds(polygon), epsilon,
                [&](std::uint32_t i) { candidates.push_back(i); return true; }, work)) return false;
        std::sort(candidates.begin(), candidates.end());
        std::vector<Polygon2> local{polygon};
        for (const std::uint32_t clipIndex : candidates) {
            if (clipIndex >= clips.size()) return false;
            std::vector<Polygon2> next;
            for (const Polygon2& fragment : local) {
                std::vector<Polygon2> difference;
                if (!SubtractTriangle(fragment, clips[clipIndex], epsilon, work, difference)) return false;
                next.insert(next.end(), std::make_move_iterator(difference.begin()),
                    std::make_move_iterator(difference.end()));
                if (next.size() + fragments.size() > MAX_ZONE_TRIANGLES) return false;
            }
            local = std::move(next);
            if (local.empty()) break;
        }
        fragments.insert(fragments.end(), std::make_move_iterator(local.begin()),
            std::make_move_iterator(local.end()));
        if (fragments.size() > MAX_ZONE_TRIANGLES) return false;
    }
    return true;
}

bool SplitByIndex(std::vector<Polygon2>& fragments, const std::vector<Triangle2>& clips,
    const TriangleIndex& index, double epsilon, std::size_t& work)
{
    std::vector<Polygon2> current;
    current.swap(fragments);
    for (const Polygon2& polygon : current) {
        std::vector<std::uint32_t> candidates;
        if (!index.Query(Bounds(polygon), epsilon,
                [&](std::uint32_t i) { candidates.push_back(i); return true; }, work)) return false;
        std::sort(candidates.begin(), candidates.end());
        std::vector<Polygon2> local{polygon};
        for (const std::uint32_t clipIndex : candidates) {
            if (clipIndex >= clips.size()) return false;
            std::vector<Polygon2> next;
            for (const Polygon2& fragment : local) {
                std::vector<Polygon2> difference;
                Polygon2 intersection;
                if (!SubtractTriangle(fragment, clips[clipIndex], epsilon, work,
                        difference, &intersection)) return false;
                next.insert(next.end(), std::make_move_iterator(difference.begin()),
                    std::make_move_iterator(difference.end()));
                if (intersection.points.size() >= 3)
                    next.push_back(std::move(intersection));
                if (next.size() + fragments.size() > MAX_ZONE_TRIANGLES) return false;
            }
            local = std::move(next);
        }
        fragments.insert(fragments.end(), std::make_move_iterator(local.begin()),
            std::make_move_iterator(local.end()));
        if (fragments.size() > MAX_ZONE_TRIANGLES) return false;
    }
    return true;
}

bool ClipMouthCorridor(const Triangle2& triangle, const WaterZoneMouthInput& mouth,
    double epsilon, std::size_t& work, std::vector<Polygon2>& output)
{
    const auto clipDistance = [&](const Polygon2& input, double threshold, bool above,
        Polygon2& result) {
        result.points.clear();
        if (input.points.empty()) return true;
        result.points.reserve(input.points.size() + 1);
        const auto distance = [&](const Point2& point) {
            return (point.x - mouth.endpoint.x) * mouth.inwardDirection.x
                + (point.z - mouth.endpoint.z) * mouth.inwardDirection.y - threshold;
        };
        for (std::size_t i = 0; i < input.points.size(); ++i) {
            if (++work > MAX_ZONE_WORK) return false;
            const Point2& current = input.points[i];
            const Point2& next = input.points[(i + 1) % input.points.size()];
            const double a = distance(current);
            const double b = distance(next);
            const bool currentInside = above ? a >= 0.0 : a <= 0.0;
            const bool nextInside = above ? b >= 0.0 : b <= 0.0;
            if (currentInside) result.points.push_back(current);
            if (currentInside != nextInside) {
                const double denominator = a - b;
                if (std::isfinite(denominator) && std::abs(denominator) > 1.0e-20) {
                    const double t = (std::clamp)(a / denominator, 0.0, 1.0);
                    result.points.push_back({current.x + (next.x - current.x) * t,
                        current.z + (next.z - current.z) * t, true});
                }
            }
        }
        CleanPolygon(result, epsilon);
        return true;
    };
    Polygon2 polygon{{triangle.points[0], triangle.points[1], triangle.points[2]}};
    Polygon2 clipped;
    if (!clipDistance(polygon, 0.0, true, clipped)) return false;
    Polygon2 bounded;
    if (!clipDistance(clipped, mouth.blendLength, false, bounded)) return false;
    if (bounded.points.size() >= 3) {
        const double area = SignedArea(bounded.points);
        if (!std::isfinite(area)) return false;
        if (area != 0.0) output.push_back(std::move(bounded));
    }
    return true;
}

bool MakeFootprintTriangle(const WaterSplineSnapshot& geometry, const WaterSplineTriangle& triangle,
    Triangle2& out, double areaEpsilon)
{
    for (std::size_t i = 0; i < 3; ++i) {
        if (triangle.indices[i] >= geometry.vertices.size()) return false;
        const auto& position = geometry.vertices[triangle.indices[i]].worldPosition;
        if (!Finite(position)) return false;
        out.points[i] = {position.x, position.z};
    }
    return NormalizeTriangle(out, areaEpsilon);
}

const std::vector<WaterSplineTriangle>& FootprintTriangles(const WaterSplineSnapshot& geometry)
{
    return geometry.footprintTriangles.empty() ? geometry.triangles : geometry.footprintTriangles;
}

struct PointKey {
    std::uint32_t x = 0;
    std::uint32_t z = 0;
    bool operator<(const PointKey& other) const
    {
        return x < other.x || (x == other.x && z < other.z);
    }
};

class PointPool {
public:
    /// @brief Stores a vertex at the float precision used by the shared output mesh.
    /// @note Distinct authored vertices still fail when they collide; only generated clipping intersections may alias an indistinguishable emitted point.
    bool Insert(const Point2& point, double mergeTolerance, std::uint32_t& out)
    {
        if (!std::isfinite(point.x) || !std::isfinite(point.z)) return false;
        float x = static_cast<float>(point.x);
        float z = static_cast<float>(point.z);
        if (!std::isfinite(x) || !std::isfinite(z)) return false;
        if (x == 0.0f) x = 0.0f;
        if (z == 0.0f) z = 0.0f;
        const PointKey key{std::bit_cast<std::uint32_t>(x), std::bit_cast<std::uint32_t>(z)};
        if (const auto it = m_lookup.find(key); it != m_lookup.end()) {
            const std::uint32_t index = it->second;
            const Point2& existing = m_exactPoints[index];
            const double distance = std::hypot(existing.x - point.x, existing.z - point.z);
            const bool mayCanonicalizeIntersection = FloatRoundoffEquivalent(existing, point)
                && (point.generatedIntersection || !m_hasAuthoredPoint[index]);
            if (distance > mergeTolerance) {
                if (!mayCanonicalizeIntersection) {
                    m_precisionCollision = true;
                    return false;
                }
            }
            if (mayCanonicalizeIntersection) {
                m_roundoffAliased[index] = true;
            }
            if (!point.generatedIntersection && !m_hasAuthoredPoint[index]) {
                m_exactPoints[index] = point;
                m_hasAuthoredPoint[index] = true;
            }
            out = index;
            return true;
        }
        if (m_points.size() >= MAX_ZONE_VERTICES) return false;
        out = static_cast<std::uint32_t>(m_points.size());
        m_points.push_back({x, z});
        m_exactPoints.push_back(point);
        m_hasAuthoredPoint.push_back(!point.generatedIntersection);
        m_roundoffAliased.push_back(false);
        m_lookup.emplace(key, out);
        return true;
    }

    [[nodiscard]] const std::vector<Point2>& Points() const { return m_points; }
    [[nodiscard]] const std::vector<Point2>& ExactPoints() const { return m_exactPoints; }
    [[nodiscard]] bool HasPrecisionCollision() const { return m_precisionCollision; }
    [[nodiscard]] bool WasRoundoffAliased(std::uint32_t index) const
    {
        return index < m_roundoffAliased.size() && m_roundoffAliased[index];
    }
    void MarkPrecisionCollision() { m_precisionCollision = true; }

private:
    std::vector<Point2> m_points;
    std::vector<Point2> m_exactPoints;
    std::vector<bool> m_hasAuthoredPoint;
    std::vector<bool> m_roundoffAliased;
    std::map<PointKey, std::uint32_t> m_lookup;
    bool m_precisionCollision = false;
};

/// @brief Adds a clipped polygon as consistently wound shared-mesh triangles.
/// @note A generated intersection wedge may collapse only when its canonical indices repeat; nonzero authored triangles remain a precision failure.
bool AddPolygonTriangles(const Polygon2& polygon, PointPool& pool, double epsilon,
    std::vector<std::array<std::uint32_t, 3>>& output, bool mergeGeneratedSlivers = false)
{
    if (polygon.points.size() < 3) return true;
    const double area = SignedArea(polygon.points);
    if (!std::isfinite(area)) return false;
    if (area == 0.0) return true;
    const bool generated = std::any_of(polygon.points.begin(), polygon.points.end(),
        [](const Point2& p) { return p.generatedIntersection; });
    if (std::abs(area) <= epsilon * epsilon) {
        if (mergeGeneratedSlivers && generated) return true;
        pool.MarkPrecisionCollision();
        return false;
    }
    std::vector<std::uint32_t> indices;
    std::vector<Point2> inputPoints = polygon.points;
    indices.reserve(polygon.points.size());
    for (const Point2& point : polygon.points) {
        std::uint32_t index = 0;
        if (!pool.Insert(point, epsilon, index)) return false;
        indices.push_back(index);
    }
    if (area < 0.0) {
        std::reverse(indices.begin(), indices.end());
        std::reverse(inputPoints.begin(), inputPoints.end());
    }
    for (std::size_t i = 1; i + 1 < indices.size(); ++i) {
        const auto& points = pool.Points();
        const Point2& a = points[indices[0]];
        const Point2& b = points[indices[i]];
        const Point2& c = points[indices[i + 1]];
        const double inputArea = Cross(inputPoints[0], inputPoints[i], inputPoints[i + 1]);
        const double exactArea = Cross(pool.ExactPoints()[indices[0]],
            pool.ExactPoints()[indices[i]], pool.ExactPoints()[indices[i + 1]]);
        if (inputArea == 0.0) continue;
        const double emittedArea = Cross(a, b, c);
        const bool generatedTriangle = inputPoints[0].generatedIntersection || inputPoints[i].generatedIntersection
            || inputPoints[i + 1].generatedIntersection;
        /// @note A clipped wedge with no positive area in the final mesh contributes no drawable coverage.
        if (mergeGeneratedSlivers && generatedTriangle && emittedArea <= epsilon * epsilon) continue;
        if (exactArea == 0.0) {
            const bool repeatedIndex = indices[0] == indices[i] || indices[i] == indices[i + 1]
                || indices[i + 1] == indices[0];
            const bool roundoffIntersectionCollapse = repeatedIndex
                && (pool.WasRoundoffAliased(indices[0]) || pool.WasRoundoffAliased(indices[i])
                    || pool.WasRoundoffAliased(indices[i + 1]))
                && (inputPoints[0].generatedIntersection || inputPoints[i].generatedIntersection
                    || inputPoints[i + 1].generatedIntersection);
            if (roundoffIntersectionCollapse) continue;
            pool.MarkPrecisionCollision();
            return false;
        }
        if (std::abs(exactArea) <= epsilon * epsilon) {
            pool.MarkPrecisionCollision();
            return false;
        }
        if (std::abs(emittedArea) <= epsilon * epsilon) {
            pool.MarkPrecisionCollision();
            return false;
        }
        output.push_back({indices[0], indices[i], indices[i + 1]});
        if (output.size() > MAX_ZONE_TRIANGLES) return false;
    }
    return true;
}

bool AddPolygonToCoverage(const Polygon2& polygon, std::vector<Triangle2>& output)
{
    if (polygon.points.size() < 3) return true;
    const double area = SignedArea(polygon.points);
    if (!std::isfinite(area)) return false;
    if (area == 0.0) return true;
    const bool reverse = area < 0.0;
    for (std::size_t i = 1; i + 1 < polygon.points.size(); ++i) {
        Triangle2 triangle{{polygon.points[0], polygon.points[i], polygon.points[i + 1]}};
        if (reverse) std::swap(triangle.points[1], triangle.points[2]);
        const double triangleArea = Cross(triangle.points[0], triangle.points[1], triangle.points[2]);
        if (!std::isfinite(triangleArea)) return false;
        if (triangleArea == 0.0) continue;
        output.push_back(triangle);
        if (output.size() > MAX_ZONE_TRIANGLES) return false;
    }
    return true;
}

bool PointOnSegment(const Point2& a, const Point2& b, const Point2& point, double epsilon, double& t)
{
    const double dx = b.x - a.x;
    const double dz = b.z - a.z;
    const double lengthSq = dx * dx + dz * dz;
    if (lengthSq <= epsilon * epsilon) return false;
    const double area = std::abs(dx * (point.z - a.z) - dz * (point.x - a.x));
    if (area > epsilon * std::sqrt(lengthSq)) return false;
    t = ((point.x - a.x) * dx + (point.z - a.z) * dz) / lengthSq;
    return t > epsilon / std::sqrt(lengthSq) && t < 1.0 - epsilon / std::sqrt(lengthSq);
}

bool SplitTJunctions(PointPool& pool, std::vector<std::array<std::uint32_t, 3>>& triangles,
    double epsilon, std::size_t& work, bool mergeGeneratedSlivers = false)
{
    std::vector<std::uint32_t> sorted(pool.Points().size());
    std::iota(sorted.begin(), sorted.end(), 0u);
    std::sort(sorted.begin(), sorted.end(), [&](std::uint32_t a, std::uint32_t b) {
        const Point2& pa = pool.ExactPoints()[a];
        const Point2& pb = pool.ExactPoints()[b];
        return pa.x < pb.x || (pa.x == pb.x && (pa.z < pb.z || (pa.z == pb.z && a < b)));
    });
    const std::vector<std::array<std::uint32_t, 3>> source = triangles;
    triangles.clear();
    triangles.reserve(source.size());
    for (const auto& triangle : source) {
        std::vector<std::uint32_t> boundary;
        bool split = false;
        std::array<std::vector<std::pair<double, std::uint32_t>>, 3> edgePoints;
        const Point2& triangleA = pool.ExactPoints()[triangle[0]];
        const Point2& triangleB = pool.ExactPoints()[triangle[1]];
        const Point2& triangleC = pool.ExactPoints()[triangle[2]];
        const double minX = (std::min)({triangleA.x, triangleB.x, triangleC.x});
        const double maxX = (std::max)({triangleA.x, triangleB.x, triangleC.x});
        const auto lower = std::lower_bound(sorted.begin(), sorted.end(), minX - epsilon,
            [&](std::uint32_t index, double x) { return pool.ExactPoints()[index].x < x; });
        for (auto it = lower; it != sorted.end(); ++it) {
            const std::uint32_t index = *it;
            const Point2& point = pool.ExactPoints()[index];
            if (point.x > maxX + epsilon) break;
            if (++work > MAX_ZONE_WORK) return false;
            if (index == triangle[0] || index == triangle[1] || index == triangle[2]) continue;
            std::size_t matchedEdge = 0;
            double matchedT = 0.0;
            std::uint32_t matchCount = 0;
            for (std::size_t edge = 0; edge < 3; ++edge) {
                double t = 0.0;
                const Point2& a = pool.ExactPoints()[triangle[edge]];
                const Point2& b = pool.ExactPoints()[triangle[(edge + 1) % 3]];
                if (!PointOnSegment(a, b, point, epsilon, t)) continue;
                matchedEdge = edge;
                matchedT = t;
                ++matchCount;
            }
            if (matchCount > 1) {
                pool.MarkPrecisionCollision();
                return false;
            }
            if (matchCount == 1) edgePoints[matchedEdge].emplace_back(matchedT, index);
        }
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const std::uint32_t start = triangle[edge];
            const std::uint32_t end = triangle[(edge + 1) % 3];
            if (edge == 0) boundary.push_back(start);
            std::sort(edgePoints[edge].begin(), edgePoints[edge].end());
            for (const auto& [t, index] : edgePoints[edge]) {
                static_cast<void>(t);
                boundary.push_back(index);
                split = true;
            }
            if (edge < 2) boundary.push_back(end);
        }
        if (!split) {
            triangles.push_back(triangle);
            continue;
        }
        const Point2& a = pool.ExactPoints()[triangle[0]];
        const Point2& b = pool.ExactPoints()[triangle[1]];
        const Point2& c = pool.ExactPoints()[triangle[2]];
        const Point2 center{(a.x + b.x + c.x) / 3.0, (a.z + b.z + c.z) / 3.0, mergeGeneratedSlivers};
        std::uint32_t centerIndex = 0;
        if (!pool.Insert(center, epsilon, centerIndex)) return false;
        if (triangles.size() + boundary.size() > MAX_ZONE_TRIANGLES) return false;
        for (std::size_t i = 0; i < boundary.size(); ++i) {
            const std::uint32_t first = boundary[i];
            const std::uint32_t second = boundary[(i + 1) % boundary.size()];
            const double area = Cross(center, pool.ExactPoints()[first], pool.ExactPoints()[second]);
            if (area == 0.0) continue;
            if (mergeGeneratedSlivers && Cross(pool.Points()[centerIndex], pool.Points()[first],
                    pool.Points()[second]) <= epsilon * epsilon) continue;
            if (std::abs(area) <= epsilon * epsilon) {
                pool.MarkPrecisionCollision();
                return false;
            }
            triangles.push_back({centerIndex, first, second});
        }
        if (triangles.size() > MAX_ZONE_TRIANGLES) return false;
    }
    return !triangles.empty();
}

math::Vector3 ToOwnerLocal(const Point2& point, float baseHeightOffset, const Transform& transform)
{
    const math::Vector3 relative{static_cast<float>(point.x) - transform.worldPosition.x,
        baseHeightOffset, static_cast<float>(point.z) - transform.worldPosition.z};
    const math::Vector3 rotated = transform.worldRotation.Inverse() * relative;
    return {rotated.x / transform.worldScale.x, rotated.y / transform.worldScale.y,
        rotated.z / transform.worldScale.z};
}

bool PreferredSource(std::size_t a, std::size_t b, std::span<const WaterZoneSource> sources)
{
    return sources[a].priority > sources[b].priority
        || (sources[a].priority == sources[b].priority && a < b);
}

/// @note Resolve only an emitted point within one float-rounding radius of an original footprint triangle.
WaterSplineSample SampleRoundedFootprint(const WaterSplineSnapshot& geometry,
    const WaterSplineTriangle& triangle, const Point2& point)
{
    const auto& a = geometry.vertices[triangle.indices[0]];
    const auto& b = geometry.vertices[triangle.indices[1]];
    const auto& c = geometry.vertices[triangle.indices[2]];
    const Point2 pa{a.worldPosition.x, a.worldPosition.z};
    const Point2 pb{b.worldPosition.x, b.worldPosition.z};
    const Point2 pc{c.worldPosition.x, c.worldPosition.z};
    const double area = Cross(pa, pb, pc);
    const double u = (std::max)(0.0, Cross(point, pb, pc) / area);
    const double v = (std::max)(0.0, Cross(pa, point, pc) / area);
    const double w = (std::max)(0.0, Cross(pa, pb, point) / area);
    const double sum = u + v + w;
    const auto interpolate = [&](float x, float y, float z) {
        return static_cast<float>((u * x + v * y + w * z) / sum);
    };
    WaterSplineSample result;
    result.baseHeightOffset = interpolate(a.baseHeightOffset, b.baseHeightOffset, c.baseHeightOffset);
    result.baseHeight = geometry.worldBaseY + result.baseHeightOffset;
    result.depth = interpolate(a.depth, b.depth, c.depth);
    result.worldVelocity = {
        interpolate(a.worldVelocity.x, b.worldVelocity.x, c.worldVelocity.x),
        interpolate(a.worldVelocity.y, b.worldVelocity.y, c.worldVelocity.y),
        interpolate(a.worldVelocity.z, b.worldVelocity.z, c.worldVelocity.z)};
    result.riverDistance = interpolate(a.riverDistance, b.riverDistance, c.riverDistance);
    result.horizonMaterialWeight = interpolate(a.horizonMaterialWeight, b.horizonMaterialWeight,
        c.horizonMaterialWeight);
    result.baseHeightGradient = triangle.baseHeightGradient;
    return result;
}

bool BuildBoundaryEdges(const std::vector<WaterSplineTriangle>& triangles,
    const std::vector<WaterSplineVertex>& vertices,
    const std::vector<Point2>& exactPoints, double epsilon,
    std::vector<std::array<std::uint32_t, 2>>& output)
{
    struct EdgeUse {
        std::uint32_t count = 0;
        std::array<std::uint32_t, 3> incidentTriangles{};
    };
    std::map<std::pair<std::uint32_t, std::uint32_t>, EdgeUse> counts;
    for (std::uint32_t triangleIndex = 0; triangleIndex < triangles.size(); ++triangleIndex) {
        const auto& triangle = triangles[triangleIndex];
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const std::uint32_t a = triangle.indices[edge];
            const std::uint32_t b = triangle.indices[(edge + 1) % 3];
            const auto key = std::minmax(a, b);
            EdgeUse& use = counts[{key.first, key.second}];
            if (use.count < use.incidentTriangles.size())
                use.incidentTriangles[use.count] = triangleIndex;
            ++use.count;
        }
    }
    output.clear();
    for (const auto& [edge, use] : counts) {
        if (use.count == 1) {
            output.push_back({edge.first, edge.second});
            continue;
        }
        if (use.count == 2) continue;
        if (edge.first >= vertices.size() || edge.second >= vertices.size()) {
            FBZZ_LOG_ERROR("WaterZone topology: edge (%u,%u) has incidence %u and invalid vertex indices (vertexCount=%zu).",
                edge.first, edge.second, use.count, vertices.size());
            return false;
        }
        const math::Vector3& a = vertices[edge.first].worldPosition;
        const math::Vector3& b = vertices[edge.second].worldPosition;
        if (edge.first >= exactPoints.size() || edge.second >= exactPoints.size()) {
            FBZZ_LOG_ERROR("WaterZone topology: edge (%u,%u) has no matching exact-position record (exactPointCount=%zu).",
                edge.first, edge.second, exactPoints.size());
            return false;
        }
        const Point2& exactA = exactPoints[edge.first];
        const Point2& exactB = exactPoints[edge.second];
        FBZZ_LOG_ERROR("WaterZone topology: edge (%u,%u) has incidence %u; float endpoints=(%.9g,%.9g)-(%.9g,%.9g), exact endpoints=(%.17g,%.17g)-(%.17g,%.17g), epsilon=%.17g, epsilonSquared=%.17g.",
            edge.first, edge.second, use.count, a.x, a.z, b.x, b.z,
            exactA.x, exactA.z, exactB.x, exactB.z, epsilon, epsilon * epsilon);
        const std::uint32_t incidentCount = (std::min)(use.count,
            static_cast<std::uint32_t>(use.incidentTriangles.size()));
        for (std::uint32_t i = 0; i < incidentCount; ++i) {
            const std::uint32_t triangleIndex = use.incidentTriangles[i];
            const auto& triangle = triangles[triangleIndex];
            const std::uint32_t i0 = triangle.indices[0];
            const std::uint32_t i1 = triangle.indices[1];
            const std::uint32_t i2 = triangle.indices[2];
            if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
                FBZZ_LOG_ERROR("WaterZone topology: incident triangle %u has invalid vertices (%u,%u,%u).",
                    triangleIndex, i0, i1, i2);
                continue;
            }
            const math::Vector3& p0 = vertices[i0].worldPosition;
            const math::Vector3& p1 = vertices[i1].worldPosition;
            const math::Vector3& p2 = vertices[i2].worldPosition;
            const double area = Cross({p0.x, p0.z}, {p1.x, p1.z}, {p2.x, p2.z});
            if (i0 >= exactPoints.size() || i1 >= exactPoints.size() || i2 >= exactPoints.size()) {
                FBZZ_LOG_ERROR("WaterZone topology: incident triangle %u has no exact-position record for vertices (%u,%u,%u).",
                    triangleIndex, i0, i1, i2);
                continue;
            }
            const Point2& q0 = exactPoints[i0];
            const Point2& q1 = exactPoints[i1];
            const Point2& q2 = exactPoints[i2];
            const double exactArea = Cross(q0, q1, q2);
            FBZZ_LOG_ERROR("WaterZone topology: incident triangle %u vertices=(%u,%u,%u), float xy=(%.9g,%.9g)(%.9g,%.9g)(%.9g,%.9g), floatArea=%.9g, exact xy=(%.17g,%.17g)(%.17g,%.17g)(%.17g,%.17g), exactArea=%.17g, epsilon=%.17g, epsilonSquared=%.17g.",
                triangleIndex, i0, i1, i2, p0.x, p0.z, p1.x, p1.z, p2.x, p2.z, area,
                q0.x, q0.z, q1.x, q1.z, q2.x, q2.z, exactArea, epsilon, epsilon * epsilon);
        }
        return false;
    }
    if (output.empty()) {
        FBZZ_LOG_ERROR("WaterZone topology: no boundary edges for %zu triangles and %zu vertices.",
            triangles.size(), vertices.size());
        return false;
    }
    return true;
}

} /// @note namespace

bool SubtractLakeIslands(WaterSplineSnapshot& lake, std::span<const WaterSplineSnapshot> islands,
    const Transform& transform)
{
    std::vector<Triangle2> source, clips;
    const auto collect = [](const WaterSplineSnapshot& mesh, std::vector<Triangle2>& triangles) {
        for (const auto& triangle : mesh.triangles) {
            Triangle2 converted;
            if (!MakeFootprintTriangle(mesh, triangle, converted, 1.0e-16)) return false;
            triangles.push_back(converted);
        }
        return true;
    };
    if (!collect(lake, source)) return false;
    for (const auto& island : islands)
        if (!collect(island, clips)) return false;
    TriangleIndex sourceIndex, clipIndex;
    if (!sourceIndex.Build(source) || !clipIndex.Build(clips)) return false;
    const double coordinateScale = (std::max)({1.0, std::abs(static_cast<double>(lake.worldBoundsMin.x)),
        std::abs(static_cast<double>(lake.worldBoundsMax.x)), std::abs(static_cast<double>(lake.worldBoundsMin.z)),
        std::abs(static_cast<double>(lake.worldBoundsMax.z))});
    const double epsilon = (std::max)(1.0e-12, coordinateScale * std::numeric_limits<double>::epsilon() * 64.0);
    PointPool pool;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::size_t work = 0;
    for (const auto& triangle : source) {
        std::vector<Polygon2> fragments{{{triangle.points[0], triangle.points[1], triangle.points[2]}}};
        if (!SubtractByIndex(fragments, clipIndex.Triangles(), clipIndex, epsilon, work)) return false;
        for (const auto& fragment : fragments)
            if (!AddPolygonTriangles(fragment, pool, epsilon, triangles)) return false;
    }
    if (!SplitTJunctions(pool, triangles, epsilon, work)
        || pool.Points().size() + lake.vertices.size() > WATER_SPLINE_MAX_VERTICES) return false;

    std::vector<WaterSplineVertex> vertices;
    vertices.reserve(pool.Points().size());
    for (std::size_t i = 0; i < pool.Points().size(); ++i) {
        const Point2& exact = pool.ExactPoints()[i];
        const Point2& emitted = pool.Points()[i];
        const Bounds2 bounds{exact.x, exact.z, exact.x, exact.z};
        WaterSplineVertex vertex;
        bool sampled = false;
        if (!sourceIndex.Query(bounds, epsilon, [&](std::uint32_t id) {
            if (sampled || !TriangleCoversWithinRounding(exact, source[id], epsilon)) return true;
            const auto& ids = lake.triangles[id].indices;
            const auto& a = lake.vertices[ids[0]];
            const auto& b = lake.vertices[ids[1]];
            const auto& c = lake.vertices[ids[2]];
            const Point2 pa{a.worldPosition.x, a.worldPosition.z};
            const Point2 pb{b.worldPosition.x, b.worldPosition.z};
            const Point2 pc{c.worldPosition.x, c.worldPosition.z};
            const double area = Cross(pa, pb, pc);
            const double wa = Cross(exact, pb, pc) / area;
            const double wb = Cross(pa, exact, pc) / area;
            const double wc = 1.0 - wa - wb;
            vertex.baseHeightOffset = static_cast<float>(wa * a.baseHeightOffset + wb * b.baseHeightOffset + wc * c.baseHeightOffset);
            vertex.depth = static_cast<float>(std::clamp(wa * a.depth + wb * b.depth + wc * c.depth,
                0.0, static_cast<double>(lake.maximumDepth)));
            vertex.worldVelocity = a.worldVelocity * static_cast<float>(wa)
                + b.worldVelocity * static_cast<float>(wb) + c.worldVelocity * static_cast<float>(wc);
            vertex.worldPosition = {static_cast<float>(emitted.x), lake.worldBaseY + vertex.baseHeightOffset,
                static_cast<float>(emitted.z)};
            vertex.localPosition = ToOwnerLocal(emitted, vertex.baseHeightOffset, transform);
            sampled = true;
            return true;
        }, work) || !sampled) return false;
        vertices.push_back(vertex);
    }
    const auto offset = static_cast<std::uint32_t>(lake.vertices.size());
    lake.vertices.insert(lake.vertices.end(), vertices.begin(), vertices.end());
    lake.triangles.clear();
    for (auto indices : triangles) {
        for (auto& index : indices) index += offset;
        lake.triangles.push_back({indices});
    }
    return true;
}

bool BuildWaterZoneSnapshot(std::span<const WaterZoneSource> sources,
    std::span<const WaterZoneMouthInput> mouths, const Transform& ownerTransform,
    float maximumWaterDepth, float maximumWorldCellSize, WaterZoneSnapshot& out,
    std::string& diagnostic, bool blendMaterials)
{
    const auto fail = [&](const char* reason) {
        diagnostic = reason;
        return false;
    };
    if (sources.empty() || sources.size() > MAX_ZONE_SOURCES) return fail("source count is outside the Zone limit");
    if (mouths.size() > MAX_ZONE_MOUTHS) return fail("river mouth count exceeds the Zone limit");
    if (!std::isfinite(maximumWaterDepth) || maximumWaterDepth < 0.0f
        || !std::isfinite(maximumWorldCellSize) || maximumWorldCellSize <= 0.0f)
        return fail("Zone depth or cell size is invalid");
    const auto ownerUp = ownerTransform.worldRotation * math::Vector3::UP;
    const double ownerRotationLength = ownerTransform.worldRotation.Length();
    if (!Finite(ownerTransform.worldPosition) || !Finite(ownerTransform.worldScale)
        || !Finite(ownerUp) || std::abs(ownerUp.x) > 0.00001f || std::abs(ownerUp.z) > 0.00001f
        || std::abs(std::abs(ownerUp.y) - 1.0f) > 0.00001f
        || !std::isfinite(ownerRotationLength) || std::abs(ownerRotationLength - 1.0) > 0.001
        || std::abs(ownerTransform.worldScale.x) < 0.0001f
        || std::abs(ownerTransform.worldScale.y) < 0.0001f
        || std::abs(ownerTransform.worldScale.z) < 0.0001f)
        return fail("owner transform is unsupported");

    std::size_t sourceTriangleCount = 0;
    std::size_t sourceVertexCount = 0;
    std::vector<std::vector<Triangle2>> sourceTriangles(sources.size());
    Bounds2 sourceBounds;
    for (std::size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex) {
        const auto& geometry = sources[sourceIndex].geometry;
        if (!geometry || geometry->vertices.empty() || geometry->triangles.empty())
            return fail("a Zone source has no valid geometry snapshot");
        const auto& footprint = FootprintTriangles(*geometry);
        sourceTriangleCount += footprint.size();
        sourceVertexCount += geometry->vertices.size();
        if (sourceTriangleCount > MAX_ZONE_TRIANGLES || sourceVertexCount > MAX_ZONE_VERTICES * 4)
            return fail("source geometry exceeds the bounded Zone input budget");
        sourceTriangles[sourceIndex].reserve(footprint.size());
        for (const auto& triangle : footprint) {
            Triangle2 converted;
            if (!MakeFootprintTriangle(*geometry, triangle, converted, 1.0e-16))
                return fail("a Zone source contains invalid footprint triangles");
            for (const auto& point : converted.points) {
                sourceBounds.minX = (std::min)(sourceBounds.minX, point.x);
                sourceBounds.minZ = (std::min)(sourceBounds.minZ, point.z);
                sourceBounds.maxX = (std::max)(sourceBounds.maxX, point.x);
                sourceBounds.maxZ = (std::max)(sourceBounds.maxZ, point.z);
            }
            sourceTriangles[sourceIndex].push_back(converted);
        }
    }
    const double extentScale = (std::max)(sourceBounds.maxX - sourceBounds.minX,
        sourceBounds.maxZ - sourceBounds.minZ);
    const double coordinateScale = (std::max)({std::abs(sourceBounds.minX), std::abs(sourceBounds.minZ),
        std::abs(sourceBounds.maxX), std::abs(sourceBounds.maxZ), extentScale});
    const double epsilon = (std::max)(1.0e-12,
        coordinateScale * std::numeric_limits<double>::epsilon() * 64.0);
    const bool followsRiver = !mouths.empty() && std::all_of(mouths.begin(), mouths.end(),
        [](const WaterZoneMouthInput& mouth) { return mouth.riverEndpointDistance >= 0.0f; });
    std::size_t work = 0;
    std::vector<TriangleIndex> sourceIndices(sources.size());
    for (std::size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex)
        if (!sourceIndices[sourceIndex].Build(sourceTriangles[sourceIndex]))
            return fail("source geometry index exceeds its bounded budget");

    std::vector<MouthGeometry> mouthGeometry;
    mouthGeometry.reserve(mouths.size());
    std::vector<std::uint32_t> sourceMouthCount(sources.size(), 0);
    std::array<bool, MAX_ZONE_SOURCES> isMouthReceiver{};
    for (const auto& mouth : mouths) {
        if (mouth.sourceIndex == 0 || mouth.sourceIndex >= sources.size()
            || mouth.receiverIndex >= sources.size() || mouth.receiverIndex == mouth.sourceIndex
            || sourceMouthCount[mouth.sourceIndex] >= 2 || !Finite(mouth.endpoint)
            || !Finite(mouth.inwardDirection) || !std::isfinite(mouth.blendLength)
            || mouth.blendLength <= 0.0f)
            return fail("a river mouth reference or blend length is invalid or ambiguous");
        const double directionLength = std::hypot(static_cast<double>(mouth.inwardDirection.x),
            static_cast<double>(mouth.inwardDirection.y));
        if (!std::isfinite(directionLength) || directionLength < 0.999 || directionLength > 1.001)
            return fail("a river mouth inward direction is not normalized");
        WaterSplineSample endpointSample;
        if (!TrySampleWaterSpline(*sources[mouth.receiverIndex].geometry, mouth.endpoint.x, mouth.endpoint.z, endpointSample))
            return fail("a river mouth endpoint is outside the receiver footprint");
        MouthGeometry built;
        built.input = mouth;
        built.receiverEndpoint = endpointSample;
        for (const Triangle2& riverTriangle : sourceTriangles[mouth.sourceIndex]) {
            std::vector<Polygon2> clipped;
            if (!ClipMouthCorridor(riverTriangle, mouth, epsilon, work, clipped))
                return fail("river mouth clipping exceeded its operation budget");
            for (const Polygon2& polygon : clipped) {
                if (!AddPolygonToCoverage(polygon, built.corridor))
                    return fail("river mouth geometry exceeds its bounded triangle budget");
            }
        }
        if (built.corridor.empty()) return fail("a river mouth corridor does not intersect its river");
        ++sourceMouthCount[mouth.sourceIndex];
        isMouthReceiver[mouth.receiverIndex] = true;
        mouthGeometry.push_back(std::move(built));
    }

    std::vector<Triangle2> allMouthTriangles;
    for (std::size_t mouthIndex = 0; mouthIndex < mouthGeometry.size(); ++mouthIndex) {
        TriangleIndex previousMouths;
        if (!previousMouths.Build(allMouthTriangles)) return fail("river mouth index exceeds its budget");
        for (const Triangle2& triangle : mouthGeometry[mouthIndex].corridor) {
            bool overlaps = false;
            if (!previousMouths.Query(Bounds(triangle), epsilon, [&](std::uint32_t candidate) {
                    Polygon2 intersection{{triangle.points[0], triangle.points[1], triangle.points[2]}};
                    Polygon2 clipped;
                    const auto& other = allMouthTriangles[candidate];
                    for (std::size_t edge = 0; edge < 3 && intersection.points.size() >= 3; ++edge) {
                        if (!ClipHalfPlane(intersection, other.points[edge], other.points[(edge + 1) % 3],
                                1.0, true, epsilon, work, clipped)) return false;
                        intersection = std::move(clipped);
                    }
                    if (intersection.points.size() >= 3) {
                        const double overlapArea = SignedArea(intersection.points);
                        if (!std::isfinite(overlapArea)) return false;
                        if (overlapArea != 0.0) overlaps = true;
                    }
                    return !overlaps;
                }, work)) return fail(overlaps ? "river mouth corridors overlap ambiguously"
                    : "river mouth intersection exceeded its operation budget");
            allMouthTriangles.push_back(triangle);
            if (allMouthTriangles.size() > MAX_ZONE_TRIANGLES) return fail("river mouth geometry exceeds its bounded triangle budget");
        }
    }
    TriangleIndex mouthIndex;
    if (!mouthIndex.Build(allMouthTriangles)) return fail("river mouth split index exceeds its budget");
    PointPool pointPool;
    std::vector<std::array<std::uint32_t, 3>> unionTriangles;
    std::vector<Triangle2> coveredTriangles;
    const auto appendPolygons = [&](const std::vector<Polygon2>& polygons) {
        for (const Polygon2& polygon : polygons) {
            if (!AddPolygonTriangles(polygon, pointPool, epsilon, unionTriangles, followsRiver)
                || !AddPolygonToCoverage(polygon, coveredTriangles)) return false;
        }
        return unionTriangles.size() <= MAX_ZONE_TRIANGLES && coveredTriangles.size() <= MAX_ZONE_TRIANGLES;
    };

    std::vector<std::size_t> sourceOrder(sources.size());
    std::iota(sourceOrder.begin(), sourceOrder.end(), 0u);
    std::stable_sort(sourceOrder.begin(), sourceOrder.end(), [&](std::size_t a, std::size_t b) {
        return PreferredSource(a, b, sources);
    });
    /// @note Corridor boundaries subdivide the winning mesh so mouth interpolation remains represented when source meshes are coarse.
    for (const std::size_t sourceIndex : sourceOrder) {
        TriangleIndex coveredIndex;
        if (!coveredIndex.Build(coveredTriangles)) return fail("union coverage index exceeds its budget");
        std::vector<Polygon2> accepted;
        /// @note Arc-distance transitions are already represented by the river's sampled cross sections.
        const bool needsMouthBoundarySplit = !followsRiver
            && (isMouthReceiver[sourceIndex] || sourceMouthCount[sourceIndex] > 0);
        for (const Triangle2& triangle : sourceTriangles[sourceIndex]) {
            std::vector<Polygon2> fragments{{{triangle.points[0], triangle.points[1], triangle.points[2]}}};
            if (needsMouthBoundarySplit && !allMouthTriangles.empty()
                && !SplitByIndex(fragments, mouthIndex.Triangles(), mouthIndex, epsilon, work))
                return fail("river mouth boundary split exceeded its bounded work budget");
            if (!SubtractByIndex(fragments, coveredIndex.Triangles(), coveredIndex, epsilon, work))
                return fail("source union exceeded its bounded work budget");
            accepted.insert(accepted.end(), std::make_move_iterator(fragments.begin()),
                std::make_move_iterator(fragments.end()));
            if (accepted.size() > MAX_ZONE_TRIANGLES) return fail("source union exceeds its triangle budget");
        }
        if (!appendPolygons(accepted)) return fail(pointPool.HasPrecisionCollision()
            ? "union geometry is below numeric tolerance or collapses at float precision"
            : "source union exceeds the bounded Zone output budget");
    }

    if (unionTriangles.empty() || pointPool.Points().size() > MAX_ZONE_VERTICES)
        return fail("Zone union has no finite output triangles");
    if (!SplitTJunctions(pointPool, unionTriangles, epsilon, work, followsRiver))
        return fail(pointPool.HasPrecisionCollision()
            ? "union geometry is below numeric tolerance or collapses at float precision"
            : "Zone T-junction split exceeded the bounded output budget");

    WaterZoneSnapshot built;
    built.geometry.shape = WaterSurfaceShape::SPLINE_LAKE;
    built.geometry.worldBaseY = ownerTransform.worldPosition.y;
    built.geometry.footprintTriangles.reserve(unionTriangles.size());
    built.geometry.triangles.reserve(unionTriangles.size());
    built.geometry.vertices.reserve(pointPool.Points().size());
    bool attributePrecisionAmbiguous = false;
    bool attributeWorkExceeded = false;
    const auto resolveAttributes = [&](const Point2& point, WaterSplineSample& resolved,
                                       WaterZoneMaterialSample& material) {
        std::array<WaterSplineSample, MAX_ZONE_SOURCES> samples{};
        std::array<bool, MAX_ZONE_SOURCES> hasSample{};
        std::size_t normalWinner = 0;
        bool anySample = false;
        const Point2 query{static_cast<float>(point.x), static_cast<float>(point.z)};
        const double roundingRadius = FloatRoundingRadius(query) * (followsRiver ? 4.0 : 1.0);
        const Bounds2 queryBounds{query.x - roundingRadius, query.z - roundingRadius,
            query.x + roundingRadius, query.z + roundingRadius};
        for (std::size_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex) {
            const auto& geometry = *sources[sourceIndex].geometry;
            hasSample[sourceIndex] = TrySampleWaterSpline(geometry, static_cast<float>(point.x),
                static_cast<float>(point.z), samples[sourceIndex]);
            if (!hasSample[sourceIndex] && followsRiver && roundingRadius > 0.0) {
                if (!sourceIndices[sourceIndex].Query(queryBounds, roundingRadius, [&](std::uint32_t candidate) {
                        if (!hasSample[sourceIndex] && TriangleCoversWithinRounding(query,
                                sourceIndices[sourceIndex].Triangles()[candidate], roundingRadius)) {
                            samples[sourceIndex] = SampleRoundedFootprint(geometry, FootprintTriangles(geometry)[candidate],
                                query);
                            hasSample[sourceIndex] = true;
                        }
                        return true;
                    }, work)) {
                    attributeWorkExceeded = true;
                    return false;
                }
            }
            if (!hasSample[sourceIndex]) continue;
            if (!anySample || PreferredSource(sourceIndex, normalWinner, sources)) normalWinner = sourceIndex;
            anySample = true;
        }
        if (!anySample) return false;

        for (std::size_t sourceIndex = 0; !followsRiver && sourceIndex < sources.size(); ++sourceIndex) {
            if (hasSample[sourceIndex]) continue;
            bool required = PreferredSource(sourceIndex, normalWinner, sources);
            if (!required && isMouthReceiver[normalWinner]) {
                for (const MouthGeometry& mouth : mouthGeometry) {
                    if (mouth.input.sourceIndex != sourceIndex || mouth.input.receiverIndex != normalWinner) continue;
                    const double dx = point.x - mouth.input.endpoint.x;
                    const double dz = point.z - mouth.input.endpoint.z;
                    const double distance = dx * mouth.input.inwardDirection.x
                        + dz * mouth.input.inwardDirection.y;
                    if (distance >= -epsilon && distance <= mouth.input.blendLength + epsilon) {
                        required = true;
                        break;
                    }
                }
            }
            if (!required || roundingRadius <= 0.0) continue;
            bool sourceBoundaryUncertain = false;
            if (!sourceIndices[sourceIndex].Query(queryBounds, roundingRadius,
                    [&](std::uint32_t candidate) {
                        sourceBoundaryUncertain = TriangleCoversWithinRounding(query,
                            sourceIndices[sourceIndex].Triangles()[candidate], roundingRadius);
                        return !sourceBoundaryUncertain;
                    }, work)) {
                if (sourceBoundaryUncertain) {
                    attributePrecisionAmbiguous = true;
                    return false;
                }
                attributeWorkExceeded = true;
                return false;
            }
        }

        const auto ownerRelativeOffset = [&](std::size_t sourceIndex, const WaterSplineSample& sample) {
            return static_cast<double>(sources[sourceIndex].geometry->worldBaseY)
                - static_cast<double>(ownerTransform.worldPosition.y) + sample.baseHeightOffset;
        };
        resolved = samples[normalWinner];
        material.receiver = material.source = static_cast<std::uint32_t>(normalWinner);
        double baseHeightOffset = ownerRelativeOffset(normalWinner, resolved);
        const MouthGeometry* selectedMouth = nullptr;
        double mouthWeight = 0.0;
        std::size_t mouthRiver = 0;
        for (const MouthGeometry& mouth : mouthGeometry) {
            const std::size_t riverIndex = mouth.input.sourceIndex;
            if (!hasSample[riverIndex]
                || (normalWinner != mouth.input.receiverIndex && normalWinner != riverIndex)) continue;
            const double dx = point.x - mouth.input.endpoint.x;
            const double dz = point.z - mouth.input.endpoint.z;
            const double distance = mouth.input.riverEndpointDistance >= 0.0f
                ? std::abs(samples[riverIndex].riverDistance - mouth.input.riverEndpointDistance)
                : dx * mouth.input.inwardDirection.x + dz * mouth.input.inwardDirection.y;
            if (distance < -epsilon || distance > mouth.input.blendLength + epsilon) continue;
            selectedMouth = &mouth;
            mouthRiver = riverIndex;
            const double t = (std::clamp)(distance / mouth.input.blendLength, 0.0, 1.0);
            mouthWeight = t * t * (3.0 - 2.0 * t);
            break;
        }
        if (selectedMouth) {
            const std::size_t receiverIndex = selectedMouth->input.receiverIndex;
            material = {static_cast<std::uint32_t>(receiverIndex), static_cast<std::uint32_t>(mouthRiver),
                static_cast<float>(mouthWeight)};
            const bool localReceiver = receiverIndex != 0 || (followsRiver
                && sources[receiverIndex].geometry->shape == WaterSurfaceShape::SPLINE_RIVER);
            const WaterSplineSample& receiver = localReceiver && hasSample[receiverIndex]
                ? samples[receiverIndex] : selectedMouth->receiverEndpoint;
            const WaterSplineSample& river = samples[mouthRiver];
            const double receiverOffset = ownerRelativeOffset(receiverIndex, receiver);
            const double riverOffset = ownerRelativeOffset(mouthRiver, river);
            baseHeightOffset = receiverOffset + (riverOffset - receiverOffset) * mouthWeight;
            resolved.depth = static_cast<float>(receiver.depth
                + (static_cast<double>(river.depth) - receiver.depth) * mouthWeight);
            resolved.worldVelocity = receiver.worldVelocity * static_cast<float>(1.0 - mouthWeight)
                + river.worldVelocity * static_cast<float>(mouthWeight);
            resolved.baseHeightGradient = receiver.baseHeightGradient * static_cast<float>(1.0 - mouthWeight)
                + river.baseHeightGradient * static_cast<float>(mouthWeight);
        }
        const double worldBaseHeight = static_cast<double>(ownerTransform.worldPosition.y) + baseHeightOffset;
        if (!std::isfinite(baseHeightOffset) || std::abs(baseHeightOffset) > std::numeric_limits<float>::max()
            || !std::isfinite(worldBaseHeight) || std::abs(worldBaseHeight) > std::numeric_limits<float>::max())
            return false;
        resolved.baseHeightOffset = static_cast<float>(baseHeightOffset);
        resolved.baseHeight = static_cast<float>(worldBaseHeight);
        resolved.horizonMaterialWeight = hasSample[0] ? samples[0].horizonMaterialWeight : 0.0f;
        resolved.depth = (std::clamp)(resolved.depth, 0.0f, maximumWaterDepth);
        return std::isfinite(resolved.depth) && Finite(resolved.worldVelocity)
            && Finite(resolved.baseHeightGradient);
    };
    constexpr auto unusedVertex = std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> vertexMap(pointPool.Points().size(), unusedVertex);
    for (const auto& triangle : unionTriangles)
        for (const auto index : triangle) vertexMap[index] = 0;
    std::vector<Point2> exactPoints;
    exactPoints.reserve(pointPool.Points().size());
    for (std::size_t i = 0; i < pointPool.Points().size(); ++i) {
        if (followsRiver && vertexMap[i] == unusedVertex) continue;
        vertexMap[i] = static_cast<std::uint32_t>(built.geometry.vertices.size());
        exactPoints.push_back(pointPool.ExactPoints()[i]);
        const Point2& point = pointPool.Points()[i];
        WaterSplineVertex vertex;
        vertex.worldPosition = {static_cast<float>(point.x), built.geometry.worldBaseY,
            static_cast<float>(point.z)};
        vertex.localPosition = ToOwnerLocal(point, 0.0f, ownerTransform);
        if (!Finite(vertex.localPosition) || !Finite(vertex.worldPosition))
            return fail("a Zone vertex cannot be represented in owner space");
        built.geometry.vertices.push_back(vertex);
    }
    for (const auto& indices : unionTriangles) {
        WaterSplineTriangle triangle;
        triangle.indices = {vertexMap[indices[0]], vertexMap[indices[1]], vertexMap[indices[2]]};
        built.geometry.triangles.push_back(triangle);
        built.geometry.footprintTriangles.push_back(triangle);
    }
    built.sourceCount = static_cast<std::uint32_t>(sources.size());
    built.mouthCount = static_cast<std::uint32_t>(mouths.size());
    if (!FinalizeWaterSplineSnapshot(built.geometry, maximumWorldCellSize))
        return fail("Zone mesh refinement or BVH finalization failed");
    if (!built.geometry.meshQualityLimited && built.geometry.maximumWorldEdgeLength > maximumWorldCellSize
        * (1.0f + 16.0f * std::numeric_limits<float>::epsilon()))
        return fail("Zone mesh refinement could not meet the requested cell size within budget");
    if (blendMaterials) built.materials.reserve(built.geometry.vertices.size());
    for (WaterSplineVertex& vertex : built.geometry.vertices) {
        const Point2 point{vertex.worldPosition.x, vertex.worldPosition.z};
        WaterSplineSample resolved;
        WaterZoneMaterialSample material;
        if (!resolveAttributes(point, resolved, material)) return fail(attributePrecisionAmbiguous
            ? "a union vertex is outside a winning source within float rounding uncertainty"
            : attributeWorkExceeded ? "Zone attribute validation exceeded its bounded work budget"
            : "a union vertex has no finite source attributes");
        vertex.worldPosition.y = resolved.baseHeight;
        vertex.localPosition = ToOwnerLocal(point, resolved.baseHeightOffset, ownerTransform);
        vertex.worldVelocity = resolved.worldVelocity;
        vertex.baseHeightOffset = resolved.baseHeightOffset;
        vertex.depth = resolved.depth;
        vertex.horizonMaterialWeight = resolved.horizonMaterialWeight;
        if (blendMaterials) built.materials.push_back(material);
        if (!Finite(vertex.localPosition) || !Finite(vertex.worldPosition) || !Finite(vertex.worldVelocity)
            || !std::isfinite(vertex.baseHeightOffset)) return fail("a Zone vertex cannot be represented in owner space");
    }
    if (!RebuildWaterSplineSnapshotDerivedData(built.geometry))
        return fail("Zone attributes or BVH could not be rebuilt after shared refinement");
    if (!HasValidWaterSplineExtents(built.geometry, ownerTransform))
        return fail("Zone local or transformed extents are not safely representable");
    if (!BuildBoundaryEdges(built.geometry.triangles, built.geometry.vertices,
            exactPoints, epsilon, built.boundaryEdges))
        return fail("Zone boundary topology is non-manifold or empty");
    built.geometry.horizonMouth = sources[0].geometry->horizonMouth;
    out = std::move(built);
    diagnostic.clear();
    return true;
}
} /// @note namespace fbzz::scene
