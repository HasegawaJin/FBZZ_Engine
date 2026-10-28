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

float MaximumEdgeLength(const WaterSplineSnapshot& snapshot)
{
    double squared = 0.0;
    for (const auto& triangle : snapshot.triangles) for (size_t edge = 0; edge < 3; ++edge) {
        const auto& a = snapshot.vertices[triangle.indices[edge]].worldPosition;
        const auto& b = snapshot.vertices[triangle.indices[(edge + 1) % 3]].worldPosition;
        const double x = static_cast<double>(a.x) - b.x;
        const double z = static_cast<double>(a.z) - b.z;
        squared = (std::max)(squared, x * x + z * z);
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
        if (snapshot.triangles.size() > MAX_TRIANGLES / 4u) { snapshot.meshQualityLimited = true; break; }
        auto vertices = snapshot.vertices;
        std::vector<WaterSplineTriangle> triangles;
        triangles.reserve(snapshot.triangles.size() * 4u);
        std::unordered_map<uint64_t, uint32_t> midpoints;
        midpoints.reserve(snapshot.triangles.size() * 2u);
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
            const uint32_t a = source.indices[0], b = source.indices[1], c = source.indices[2];
            const uint32_t ab = midpointIndex(a, b), bc = midpointIndex(b, c), ca = midpointIndex(c, a);
            if (overBudget) break;
            triangles.push_back({{a, ab, ca}, source.baseHeightGradient});
            triangles.push_back({{ab, b, bc}, source.baseHeightGradient});
            triangles.push_back({{ca, bc, c}, source.baseHeightGradient});
            triangles.push_back({{ab, bc, ca}, source.baseHeightGradient});
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
