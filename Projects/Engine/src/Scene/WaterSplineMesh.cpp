/// @file    WaterSplineMesh.cpp
/// @brief   水域三角形の共有中点による細分化と実効セル幅を求める。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <Engine/Scene/WaterSplineMesh.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace fbzz::scene {
namespace {
constexpr size_t MAX_TRIANGLES = 65536;
constexpr unsigned MAX_REFINEMENT_LEVELS = 10;

double EdgeLengthSquared(const WaterSplineVertex& a, const WaterSplineVertex& b)
{
    const double x = static_cast<double>(a.worldPosition.x) - b.worldPosition.x;
    const double z = static_cast<double>(a.worldPosition.z) - b.worldPosition.z;
    return x * x + z * z;
}

float MaximumEdgeLength(const WaterSplineSnapshot& snapshot)
{
    double squared = 0.0;
    for (const auto& triangle : snapshot.triangles) for (size_t edge = 0; edge < 3; ++edge) {
        const auto& a = snapshot.vertices[triangle.indices[edge]];
        const auto& b = snapshot.vertices[triangle.indices[(edge + 1) % 3]];
        squared = (std::max)(squared, EdgeLengthSquared(a, b));
    }
    return static_cast<float>((std::min)(std::sqrt(squared), static_cast<double>(std::numeric_limits<float>::max())));
}

WaterSplineVertex Midpoint(const WaterSplineVertex& a, const WaterSplineVertex& b)
{
    WaterSplineVertex vertex;
    vertex.localPosition = a.localPosition * 0.5f + b.localPosition * 0.5f;
    vertex.worldPosition = a.worldPosition * 0.5f + b.worldPosition * 0.5f;
    vertex.worldVelocity = a.worldVelocity * 0.5f + b.worldVelocity * 0.5f;
    vertex.baseHeightOffset = a.baseHeightOffset * 0.5f + b.baseHeightOffset * 0.5f;
    vertex.depth = a.depth * 0.5f + b.depth * 0.5f;
    vertex.riverDistance = a.riverDistance * 0.5f + b.riverDistance * 0.5f;
    vertex.horizonMaterialWeight = a.horizonMaterialWeight * 0.5f + b.horizonMaterialWeight * 0.5f;
    return vertex;
}

bool ValidTriangle(const std::vector<WaterSplineVertex>& vertices, WaterSplineTriangle& triangle)
{
    const auto& a = vertices[triangle.indices[0]];
    const auto& b = vertices[triangle.indices[1]];
    const auto& c = vertices[triangle.indices[2]];
    const double bx = static_cast<double>(b.worldPosition.x) - a.worldPosition.x;
    const double bz = static_cast<double>(b.worldPosition.z) - a.worldPosition.z;
    const double cx = static_cast<double>(c.worldPosition.x) - a.worldPosition.x;
    const double cz = static_cast<double>(c.worldPosition.z) - a.worldPosition.z;
    const double determinant = bx * cz - bz * cx;
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12) return false;
    const double bh = static_cast<double>(b.baseHeightOffset) - a.baseHeightOffset;
    const double ch = static_cast<double>(c.baseHeightOffset) - a.baseHeightOffset;
    const double dx = (bh * cz - ch * bz) / determinant;
    const double dz = (bx * ch - cx * bh) / determinant;
    if (!std::isfinite(dx) || !std::isfinite(dz)
        || std::abs(dx) > std::numeric_limits<float>::max() || std::abs(dz) > std::numeric_limits<float>::max()) return false;
    triangle.baseHeightGradient = {static_cast<float>(dx), static_cast<float>(dz)};
    return true;
}
} /// @note namespace

void RefineWaterSplineMesh(WaterSplineSnapshot& snapshot, float maximumWorldCellSize)
{
    assert(std::isfinite(maximumWorldCellSize) && maximumWorldCellSize > 0.0f);
    snapshot.meshQualityLimited = false;
    snapshot.maximumWorldEdgeLength = MaximumEdgeLength(snapshot);
    for (unsigned level = 0; level < MAX_REFINEMENT_LEVELS
        && snapshot.maximumWorldEdgeLength > maximumWorldCellSize; ++level) {
        /// @note Spend the remaining budget on longer edges before attempting the final target spacing.
        const double edgeLimit = (std::max)(static_cast<double>(maximumWorldCellSize),
            0.5 * static_cast<double>(snapshot.maximumWorldEdgeLength));
        const double limitSquared = edgeLimit * edgeLimit;
        auto vertices = snapshot.vertices;
        std::vector<WaterSplineTriangle> triangles;
        triangles.reserve((std::min)(snapshot.triangles.size() * 2u, MAX_TRIANGLES));
        std::unordered_map<uint64_t, uint32_t> midpoints;
        bool overBudget = false;
        const auto midpointIndex = [&](uint32_t a, uint32_t b) {
            const uint64_t key = (static_cast<uint64_t>((std::min)(a, b)) << 32u) | (std::max)(a, b);
            if (const auto found = midpoints.find(key); found != midpoints.end()) return found->second;
            if (vertices.size() >= WATER_SPLINE_MAX_VERTICES) { overBudget = true; return uint32_t{0}; }
            const auto index = static_cast<uint32_t>(vertices.size());
            vertices.push_back(Midpoint(snapshot.vertices[a], snapshot.vertices[b]));
            midpoints.emplace(key, index);
            return index;
        };
        for (const auto& source : snapshot.triangles) {
            /// @note Adjacent triangles make the same split decision for their shared edge.
            bool split[3]{};
            size_t splitCount = 0;
            for (size_t edge = 0; edge < 3; ++edge) {
                const auto& a = snapshot.vertices[source.indices[edge]];
                const auto& b = snapshot.vertices[source.indices[(edge + 1) % 3]];
                split[edge] = EdgeLengthSquared(a, b) > limitSquared;
                splitCount += split[edge] ? 1u : 0u;
            }
            if (triangles.size() + splitCount + 1 > MAX_TRIANGLES) { overBudget = true; break; }
            if (splitCount == 0) {
                triangles.push_back(source);
                continue;
            }

            /// @note Rotate the indices so that the split edges start at AB, then BC, then CA.
            size_t first = 0;
            if (splitCount == 1) {
                while (!split[first]) ++first;
            } else if (splitCount == 2) {
                while (!split[first] || !split[(first + 1) % 3]) ++first;
            }
            const uint32_t a = source.indices[first];
            const uint32_t b = source.indices[(first + 1) % 3];
            const uint32_t c = source.indices[(first + 2) % 3];
            const uint32_t ab = midpointIndex(a, b);
            const uint32_t bc = splitCount >= 2 ? midpointIndex(b, c) : 0;
            const uint32_t ca = splitCount == 3 ? midpointIndex(c, a) : 0;
            if (overBudget) break;
            const auto emit = [&](uint32_t x, uint32_t y, uint32_t z) {
                triangles.push_back({{x, y, z}, source.baseHeightGradient});
            };
            if (splitCount == 1) {
                emit(a, ab, c);
                emit(ab, b, c);
            } else if (splitCount == 2) {
                emit(ab, b, bc);
                if (EdgeLengthSquared(vertices[a], vertices[bc]) < EdgeLengthSquared(vertices[ab], vertices[c])) {
                    emit(a, ab, bc);
                    emit(a, bc, c);
                } else {
                    emit(a, ab, c);
                    emit(ab, bc, c);
                }
            } else {
                emit(a, ab, ca);
                emit(ab, b, bc);
                emit(ca, bc, c);
                emit(ab, bc, ca);
            }
        }
        if (overBudget) { snapshot.meshQualityLimited = true; break; }
        /// @note 大きなworld原点で中点が端点へ丸められる場合は、最後の可逆な全段階を保持する。
        bool validLevel = true;
        for (auto& triangle : triangles) {
            if (!ValidTriangle(vertices, triangle)) { validLevel = false; break; }
        }
        if (!validLevel) {
            snapshot.meshQualityLimited = true;
            break;
        }
        snapshot.vertices = std::move(vertices);
        snapshot.triangles = std::move(triangles);
        snapshot.maximumWorldEdgeLength = MaximumEdgeLength(snapshot);
    }
    if (snapshot.maximumWorldEdgeLength > maximumWorldCellSize) snapshot.meshQualityLimited = true;
}
} /// @note namespace fbzz::scene
