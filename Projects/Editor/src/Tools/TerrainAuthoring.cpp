/// @file    TerrainAuthoring.cpp
/// @brief   高さ画像の部分貼り付けと草地・岩肌・積雪の確定処理。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include "TerrainAuthoring.hpp"
#include "TerrainBrush.hpp"
#include <Editor/Op/OpData.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/TerrainSplat.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
#include <queue>
#include <set>
#include <utility>
#include <vector>

namespace fbzz::editor {
namespace {

constexpr std::size_t MAX_SHORE_POINTS = 256;
constexpr std::array<std::pair<int, int>, 6> TERRAIN_NEIGHBORS{{{1,0}, {-1,0}, {0,1}, {0,-1}, {1,-1}, {-1,1}}};

float Transition(float value, float center, float width)
{
    if (width == 0.0f) return value >= center ? 1.0f : 0.0f;
    const float t = std::clamp((value - center) / width + 0.5f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

struct TerrainAnalysisGrid {
    int nx = 0, nz = 0;
    float stepX = 0.0f, stepZ = 0.0f;
    math::Vector3 center;
    std::vector<float> ground;

    int Stride() const { return nx + 1; }
    int CenterIndex() const { return nz / 2 * Stride() + nx / 2; }
    bool Boundary(int index) const
    {
        return index % Stride() == 0 || index % Stride() == nx || index / Stride() == 0 || index / Stride() == nz;
    }
    math::Vector3 Position(int index) const
    {
        return {center.x + (index % Stride() - nx * 0.5f) * stepX, center.y,
            center.z + (index / Stride() - nz * 0.5f) * stepZ};
    }
};

bool SampleTerrainAnalysisGrid(std::span<const TerrainAnalysisSurface> surfaces,
    const TerrainShoreSettings& settings, TerrainAnalysisGrid& grid)
{
    const auto& [seed, width, depth, spacing, tolerance] = settings;
    if (!Finite(seed) || !std::isfinite(width) || !std::isfinite(depth) || !std::isfinite(spacing)
        || !std::isfinite(tolerance) || width <= 0.0f || depth <= 0.0f || spacing <= 0.0f || tolerance < 0.0f
        || width / static_cast<double>(spacing) > 256.0 || depth / static_cast<double>(spacing) > 256.0
        || surfaces.empty() || surfaces.size() > 16)
        return false;
    grid.nx = 2 * static_cast<int>(std::ceil(width / (2.0 * spacing)));
    grid.nz = 2 * static_cast<int>(std::ceil(depth / (2.0 * spacing)));
    grid.stepX = width / grid.nx;
    grid.stepZ = depth / grid.nz;
    grid.center = seed;
    grid.ground.assign(static_cast<std::size_t>(grid.Stride()) * (grid.nz + 1), std::numeric_limits<float>::quiet_NaN());
    const int last = static_cast<int>(grid.ground.size() - 1);
    if (!Finite(grid.Position(0)) || !Finite(grid.Position(last))
        || grid.Position(0).x >= grid.Position(1).x || grid.Position(0).z >= grid.Position(grid.Stride()).z
        || grid.Position(last - 1).x >= grid.Position(last).x
        || grid.Position(last - grid.Stride()).z >= grid.Position(last).z) return false;
    for (int i = 0; i < static_cast<int>(grid.ground.size()); ++i) {
        const auto point = grid.Position(i);
        for (const auto& surface : surfaces) {
            scene::TerrainSurfaceSample sample;
            if (scene::TrySampleTerrainWorld(*surface.terrain, *surface.pose, point.x, point.z, sample)
                && (!std::isfinite(grid.ground[i]) || sample.position.y > grid.ground[i]))
                grid.ground[i] = sample.position.y;
        }
    }
    return true;
}

float SmoothUnit(float value)
{
    const float t = std::clamp(value, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

double SegmentDistanceSquared(const math::Vector3& point, const math::Vector3& first, const math::Vector3& last)
{
    const double dx = static_cast<double>(last.x) - first.x, dz = static_cast<double>(last.z) - first.z;
    const double x = static_cast<double>(point.x) - first.x, z = static_cast<double>(point.z) - first.z;
    const double length = dx * dx + dz * dz;
    const double t = length > 0.0 ? std::clamp((x * dx + z * dz) / length, 0.0, 1.0) : 0.0;
    return (x - t * dx) * (x - t * dx) + (z - t * dz) * (z - t * dz);
}

std::vector<math::Vector3> SimplifyShore(std::vector<math::Vector3>& ring, float tolerance)
{
    const auto size = ring.size();
    if (size < 3) return {};
    const auto split = static_cast<std::size_t>(std::max_element(ring.begin(), ring.end(), [&](const auto& a, const auto& b) {
        return SegmentDistanceSquared(a, ring.front(), ring.front()) < SegmentDistanceSquared(b, ring.front(), ring.front());
    }) - ring.begin());
    if (split == 0) return {};
    ring.push_back(ring.front());
    std::vector<bool> keep(size + 1, false);
    keep[0] = keep[split] = true;
    std::size_t kept = 2;
    std::vector<std::pair<std::size_t, std::size_t>> pending{{0, split}, {split, size}};
    const double limit = static_cast<double>(tolerance) * tolerance;
    while (!pending.empty()) {
        const auto [first, last] = pending.back();
        pending.pop_back();
        double largest = limit;
        std::size_t furthest = first;
        for (auto i = first + 1; i < last; ++i) {
            const double distance = SegmentDistanceSquared(ring[i], ring[first], ring[last]);
            if (distance > largest) { largest = distance; furthest = i; }
        }
        if (furthest != first) {
            if (++kept > MAX_SHORE_POINTS) return {};
            keep[furthest] = true;
            pending.emplace_back(first, furthest);
            pending.emplace_back(furthest, last);
        }
    }
    std::vector<math::Vector3> result;
    for (std::size_t i = 0; i < size; ++i)
        if (keep[i]) result.push_back(ring[i]);
    return result;
}

} /// @note namespace

bool SculptTerrainRidge(scene::TerrainComponent& terrain, const scene::Transform& pose, const TerrainRidgeSettings& settings)
{
    const auto& [start, end, height, leftWidth, rightWidth, bend, peakAlong] = settings;
    math::Vector3 normal;
    const auto up = pose.worldRotation * math::Vector3::UP;
    if (!Finite(start) || !Finite(end) || !Finite(pose.worldPosition)
        || !scene::TryTerrainNormalToWorld(pose, math::Vector3::UP, normal)
        || std::abs(up.x) > 0.00001f || std::abs(up.z) > 0.00001f
        || !std::isfinite(height) || !std::isfinite(bend) || !std::isfinite(peakAlong)
        || !std::isfinite(leftWidth) || !std::isfinite(rightWidth) || leftWidth <= 0.0f || rightWidth <= 0.0f
        || peakAlong < 0.1f || peakAlong > 0.9f || terrain.columns < 3 || terrain.rows < 3
        || terrain.columns > 4097 || terrain.rows > 4097 || terrain.heightData.size() != terrain.VertexCount()
        || !std::isfinite(terrain.cellSize) || terrain.cellSize <= 0.0f
        || !std::isfinite(terrain.maxHeight) || terrain.maxHeight <= 0.0f) return false;
    const double axisX = static_cast<double>(end.x) - start.x, axisZ = static_cast<double>(end.z) - start.z;
    const double length = std::hypot(axisX, axisZ);
    if (length < 1.0) return false;
    const double dirX = axisX / length, dirZ = axisZ / length;
    const float margin = (std::max)(leftWidth, rightWidth) + std::abs(bend);
    float minX = std::numeric_limits<float>::infinity(), minZ = minX;
    float maxX = -minX, maxZ = -minX;
    for (const float x : {(std::min)(start.x, end.x) - margin, (std::max)(start.x, end.x) + margin}) {
        for (const float z : {(std::min)(start.z, end.z) - margin, (std::max)(start.z, end.z) + margin}) {
            const auto local = ToTerrainLocal(pose, {x, pose.worldPosition.y, z});
            if (!Finite(local)) return false;
            minX = (std::min)(minX, local.x); maxX = (std::max)(maxX, local.x);
            minZ = (std::min)(minZ, local.z); maxZ = (std::max)(maxZ, local.z);
        }
    }
    const auto grid = [&](float value, int dimension) {
        return static_cast<int>(std::clamp(static_cast<double>(value) / terrain.cellSize, 1.0, static_cast<double>(dimension - 2)));
    };
    const int x0 = grid(minX, terrain.columns), x1 = grid(maxX, terrain.columns);
    const int z0 = grid(minZ, terrain.rows), z1 = grid(maxZ, terrain.rows);
    if (static_cast<std::size_t>(x1 - x0 + 1) * (z1 - z0 + 1) > 1048576) return false;
    const double heightScale = static_cast<double>(terrain.maxHeight) * pose.worldScale.y * up.y;
    const auto matrix = pose.GetWorldMatrix();
    std::vector<std::pair<std::size_t, float>> changes;
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            const auto point = matrix * math::Vector4{x * terrain.cellSize, 0.0f, z * terrain.cellSize, 1.0f};
            const double dx = static_cast<double>(point.x) - start.x, dz = static_cast<double>(point.z) - start.z;
            const double t = (dx * dirX + dz * dirZ) / length;
            if (!std::isfinite(t)) return false;
            if (t <= 0.0 || t >= 1.0) continue;
            const double side = -dx * dirZ + dz * dirX - 4.0 * bend * t * (1.0 - t);
            const double width = side >= 0.0 ? leftWidth : rightWidth;
            if (std::abs(side) >= width) continue;
            const float along = SmoothUnit(static_cast<float>((std::min)(t / peakAlong, (1.0 - t) / (1.0 - peakAlong))));
            const float across = SmoothUnit(static_cast<float>(1.0 - std::abs(side) / width));
            const auto index = static_cast<std::size_t>(z) * terrain.columns + x;
            const float before = terrain.heightData[index];
            if (!std::isfinite(before)) return false;
            const float after = static_cast<float>(std::clamp(before + height * along * across / heightScale, -1.0, 1.0));
            if (after != before) changes.emplace_back(index, after);
        }
    }
    for (const auto& [index, heightValue] : changes) terrain.heightData[index] = heightValue;
    return true;
}

bool SculptTerrainMassif(scene::TerrainComponent& terrain, const scene::Transform& pose, const TerrainMassifSettings& settings)
{
    const auto& spine = settings.spine;
    const double dx = static_cast<double>(spine.end.x) - spine.start.x;
    const double dz = static_cast<double>(spine.end.z) - spine.start.z;
    const double length = std::hypot(dx, dz);
    if (!std::isfinite(length) || length < 8.0 || length > 20000.0
        || !std::isfinite(spine.height) || spine.height <= 0.0f || spine.height > 10000.0f
        || !std::isfinite(spine.leftWidth) || spine.leftWidth < 4.0f || spine.leftWidth > 4000.0f
        || !std::isfinite(spine.rightWidth) || spine.rightWidth < 4.0f || spine.rightWidth > 4000.0f
        || settings.branchesPerSide < 2 || settings.branchesPerSide > 4
        || !std::isfinite(settings.erosion) || settings.erosion < 0.0f || settings.erosion > 1.0f) return false;

    auto before = terrain.heightData;
    const auto restore = [&] {
        terrain.heightData.swap(before);
        return false;
    };
    if (!SculptTerrainRidge(terrain, pose, spine)) return restore();
    const math::Vector3 side{static_cast<float>(-dz / length), 0.0f, static_cast<float>(dx / length)};
    const auto center = [&](float t) {
        return spine.start + (spine.end - spine.start) * t + side * (4.0f * spine.bend * t * (1.0f - t));
    };
    std::uint32_t state = settings.seed;
    const auto variation = [&] {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    };
    std::vector<std::pair<math::Vector3, float>> erosionSites;
    for (const float sign : {1.0f, -1.0f}) {
        const float width = sign > 0.0f ? spine.leftWidth : spine.rightWidth;
        const auto offset = side * sign;
        const float step = 0.58f / settings.branchesPerSide;
        std::array<TerrainRidgeSettings, 4> branches;
        for (int i = 0; i < settings.branchesPerSide; ++i) {
            const float t = 0.18f + step * (i + 0.45f + 0.3f * (variation() - 0.5f));
            auto& branch = branches[i];
            branch.start = center(t) + offset * (0.1f * width);
            branch.end = center(t + std::lerp(0.10f, 0.22f, variation()))
                + offset * (width * std::lerp(0.95f, 1.35f, variation()));
            branch.height = spine.height * std::lerp(0.40f, 0.65f, variation());
            branch.leftWidth = width * std::lerp(0.16f, 0.26f, variation());
            branch.rightWidth = width * std::lerp(0.16f, 0.26f, variation());
            branch.bend = sign * width * std::lerp(-0.08f, 0.08f, variation());
            if (!SculptTerrainRidge(terrain, pose, branch)) return restore();
        }
        for (int i = 1; i < settings.branchesPerSide; ++i) {
            const auto& previous = branches[i - 1];
            const auto& branch = branches[i];
            TerrainRidgeSettings valley;
            valley.start = (previous.start + branch.start) * 0.5f + offset * (0.18f * width);
            valley.end = (previous.end + branch.end) * 0.5f + offset * (0.22f * width);
            valley.height = -spine.height * std::lerp(0.12f, 0.22f, variation());
            valley.leftWidth = width * 0.18f;
            valley.rightWidth = width * 0.18f;
            valley.peakAlong = 0.68f;
            if (!SculptTerrainRidge(terrain, pose, valley)) return restore();
            erosionSites.emplace_back(valley.start + (valley.end - valley.start) * 0.55f, width * 0.22f);
        }
    }
    if (settings.erosion > 0.0f) {
        const float scale = (std::max)(std::abs(pose.worldScale.x), std::abs(pose.worldScale.z));
        std::uint32_t stroke = 0;
        for (const auto& [point, radius] : erosionSites) {
            TerrainBrush brush;
            brush.radius = radius / scale;
            brush.strength = settings.erosion;
            brush.seed = settings.seed;
            const auto local = ToTerrainLocal(pose, point);
            if (!Finite(local) || !std::isfinite(brush.radius)) return restore();
            if (BrushOverlapsTerrainXZ(terrain, local, brush.radius))
                ApplyTerrainSculpt(terrain, local, brush, TerrainSculptOp::HydraulicErosion, 0.0f, 1.0f / 30.0f, stroke);
            ++stroke;
        }
    }
    const auto restoreVertex = [&](int x, int z) {
        const auto index = static_cast<std::size_t>(z) * terrain.columns + x;
        terrain.heightData[index] = before[index];
    };
    for (int x = 0; x < terrain.columns; ++x) {
        restoreVertex(x, 0);
        restoreVertex(x, terrain.rows - 1);
    }
    for (int z = 1; z < terrain.rows - 1; ++z) {
        restoreVertex(0, z);
        restoreVertex(terrain.columns - 1, z);
    }
    if (terrain.CountHoles() != 0)
        for (int z = 0; z < terrain.rows - 1; ++z)
            for (int x = 0; x < terrain.columns - 1; ++x)
                if (terrain.IsHoleCell(x, z)) {
                    restoreVertex(x, z);
                    restoreVertex(x + 1, z);
                    restoreVertex(x, z + 1);
                    restoreVertex(x + 1, z + 1);
                }
    return true;
}

bool PlanTerrainRiverCarve(const scene::TerrainComponent& terrain, const scene::Transform& pose,
    std::span<const TerrainRiverCarveSection> sections, const TerrainRiverCarveSettings& settings, TerrainRiverCarvePlan& out,
    bool includeBoundary)
{
    constexpr std::size_t MAX_VERTEX_VISITS = 2'000'000;
    const auto validRange = [](float value, float minimum, float maximum) {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    };
    const auto up = pose.worldRotation * math::Vector3::UP;
    math::Vector3 normal;
    if (sections.size() < 2 || sections.size() > 8192 || terrain.columns < 3 || terrain.rows < 3
        || terrain.columns > 4097 || terrain.rows > 4097 || terrain.heightData.size() != terrain.VertexCount()
        || !validRange(terrain.cellSize, 0.001f, 100000.0f) || !validRange(terrain.maxHeight, 0.001f, 1000000.0f)
        || !Finite(pose.worldPosition) || !Finite(up) || std::abs(up.x) > 0.00001f || std::abs(up.z) > 0.00001f
        || !scene::TryTerrainNormalToWorld(pose, math::Vector3::UP, normal)
        || !validRange(settings.bankWidth, 0.0f, 512.0f) || !validRange(settings.bankRise, 0.0f, 100.0f)
        || !validRange(settings.endFade, 0.0f, 512.0f) || !validRange(settings.maxCut, 0.01f, 1000.0f)
        || !validRange(settings.gentleGrade, 0.0f, 1.0f) || !validRange(settings.depositHeight, 0.0f, 10.0f)) return false;
    if (settings.bankLayer < -1 || settings.bankLayer >= terrain.LayerCount()
        || settings.protectedLayer < -1 || settings.protectedLayer >= terrain.LayerCount()
        || settings.gentleBankLayer < -1 || settings.gentleBankLayer >= terrain.LayerCount()
        || (settings.protectedLayer >= 0 && (settings.bankLayer == settings.protectedLayer || settings.gentleBankLayer == settings.protectedLayer))
        || ((settings.bankLayer >= 0 || settings.gentleBankLayer >= 0 || settings.protectedLayer >= 0) && !terrain.HasValidSplat())) return false;
    const double heightScale = static_cast<double>(terrain.maxHeight) * pose.worldScale.y * up.y;
    if (!std::isfinite(heightScale) || heightScale <= 0.0) return false;
    std::vector<double> distanceAlong(sections.size());
    std::vector<std::size_t> reachEnds(sections.size());
    std::size_t reachStart = 0;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const auto& section = sections[i];
        if (!std::isfinite(section.center.x) || !std::isfinite(section.center.y) || !std::isfinite(section.waterHeight)
            || !validRange(section.halfWidth, 0.01f, 4096.0f) || !validRange(section.depth, 0.0f, 10000.0f))
            return false;
        if (i == 0) continue;
        if (section.beginsReach) {
            if (i - reachStart < 2) return false;
            std::fill(reachEnds.begin() + reachStart, reachEnds.begin() + i, i - 1);
            reachStart = i;
            continue;
        }
        const auto& previous = sections[i - 1];
        const double length = std::hypot(static_cast<double>(section.center.x) - previous.center.x,
            static_cast<double>(section.center.y) - previous.center.y);
        if (length < 0.0001) return false;
        distanceAlong[i] = distanceAlong[i - 1] + length;
    }
    if (sections.size() - reachStart < 2) return false;
    std::fill(reachEnds.begin() + reachStart, reachEnds.end(), sections.size() - 1);

    struct Target { float cutHeight, depositHeight, bankWeight; int bankLayer; double waterOffset; bool protectedVertex; };
    std::map<std::size_t, Target> targets;
    std::size_t visits = 0;
    const auto matrix = pose.GetWorldMatrix();
    /// @note A narrow bed between grid vertices needs support from the triangles that interpolate its height.
    const double cellSupport = 0.5 * terrain.cellSize * std::hypot(pose.worldScale.x, pose.worldScale.z);
    reachStart = 0;
    for (std::size_t i = 1; i < sections.size(); ++i) {
        const auto& a = sections[i - 1];
        const auto& b = sections[i];
        if (b.beginsReach) { reachStart = i; continue; }
        const auto reachEnd = reachEnds[i];
        const float radius = std::max({a.halfWidth, b.halfWidth, static_cast<float>(cellSupport)}) + settings.bankWidth;
        float minX = std::numeric_limits<float>::infinity(), minZ = minX, maxX = -minX, maxZ = -minX;
        for (const float x : {(std::min)(a.center.x, b.center.x) - radius, (std::max)(a.center.x, b.center.x) + radius}) {
            for (const float z : {(std::min)(a.center.y, b.center.y) - radius, (std::max)(a.center.y, b.center.y) + radius}) {
                const auto local = ToTerrainLocal(pose, {x, pose.worldPosition.y, z});
                if (!Finite(local)) return false;
                minX = (std::min)(minX, local.x);
                maxX = (std::max)(maxX, local.x);
                minZ = (std::min)(minZ, local.z);
                maxZ = (std::max)(maxZ, local.z);
            }
        }
        const double cell = terrain.cellSize;
        if (maxX < 0 || maxZ < 0 || minX > (terrain.columns - 1) * cell || minZ > (terrain.rows - 1) * cell) continue;
        const auto bound = [cell](double value, int count) {
            return static_cast<int>(std::clamp(value / cell, 0.0, static_cast<double>(count - 1)));
        };
        const int x0 = bound(minX, terrain.columns), x1 = bound(maxX, terrain.columns);
        const int z0 = bound(minZ, terrain.rows), z1 = bound(maxZ, terrain.rows);
        visits += static_cast<std::size_t>(x1 - x0 + 1) * (z1 - z0 + 1);
        if (visits > MAX_VERTEX_VISITS) return false;
        const double dx = static_cast<double>(b.center.x) - a.center.x;
        const double dz = static_cast<double>(b.center.y) - a.center.y;
        const double lengthSquared = dx * dx + dz * dz;
        const double length = std::sqrt(lengthSquared);
        const double startPadding = (std::max)(0.0, cellSupport - a.halfWidth) / length;
        const double endPadding = (std::max)(0.0, cellSupport - b.halfWidth) / length;
        const bool gentle = std::abs(static_cast<double>(b.waterHeight) - a.waterHeight) <= settings.gentleGrade * length;
        const int bankLayer = gentle && settings.gentleBankLayer >= 0 ? settings.gentleBankLayer : settings.bankLayer;
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                const auto point = matrix * math::Vector4{x * terrain.cellSize, 0, z * terrain.cellSize, 1};
                if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) return false;
                const double rx = static_cast<double>(point.x) - a.center.x;
                const double rz = static_cast<double>(point.z) - a.center.y;
                const double projection = (rx * dx + rz * dz) / lengthSquared;
                const auto& start = sections[reachStart];
                if ((i == reachStart + 1 && !start.openStart && projection < -startPadding)
                    || (i == reachEnd && !start.openEnd && projection > 1.0 + endPadding)) continue;
                const double t = std::clamp(projection, 0.0, 1.0);
                const double halfWidth = std::lerp(static_cast<double>(a.halfWidth), static_cast<double>(b.halfWidth), t);
                const double support = (std::max)(0.0, cellSupport - halfWidth);
                const double lateral = (std::max)(0.0, std::hypot(rx - t * dx, rz - t * dz) - support);
                if (lateral > halfWidth + settings.bankWidth) continue;
                const double water = std::lerp(static_cast<double>(a.waterHeight), static_cast<double>(b.waterHeight), t);
                const double depth = std::lerp(static_cast<double>(a.depth), static_cast<double>(b.depth), t);
                const double heightCap = point.y + heightScale;
                double target = water - depth * (1.0 - SmoothUnit(static_cast<float>(lateral / halfWidth)));
                if (lateral > halfWidth) {
                    const double edge = (lateral - halfWidth) / settings.bankWidth;
                    const double blend = edge * edge;
                    target = std::lerp(water + settings.bankRise * SmoothUnit(static_cast<float>(edge)), heightCap, blend * blend);
                }
                float endWeight = 1.0f;
                if (settings.endFade > 0.0f) {
                    const double station = std::lerp(distanceAlong[i - 1], distanceAlong[i], t);
                    const double startDistance = start.openStart ? settings.endFade : station;
                    const double endDistance = start.openEnd ? settings.endFade : distanceAlong[reachEnd] - station;
                    const double toEnd = (std::min)(startDistance, endDistance);
                    endWeight = SmoothUnit(static_cast<float>(toEnd / settings.endFade));
                    target = std::lerp(heightCap, target, static_cast<double>(endWeight));
                }
                const auto index = static_cast<std::size_t>(z) * terrain.columns + x;
                const float before = terrain.heightData[index];
                if (!std::isfinite(before)) return false;
                const double normalized = (target - point.y) / heightScale;
                const float height = static_cast<float>((std::min)(normalized, static_cast<double>(before)));
                float bankWeight = 0.0f;
                const float across = lateral <= halfWidth
                    ? SmoothUnit(static_cast<float>((lateral / halfWidth - 0.55) / 0.45))
                    : 1.0f - SmoothUnit(static_cast<float>((lateral - halfWidth) / settings.bankWidth));
                if (bankLayer >= 0) bankWeight = across * endWeight;
                float deposit = before;
                const double oldWorld = point.y + before * heightScale;
                if (gentle && settings.depositHeight > 0.0f && lateral > halfWidth && oldWorld >= water && height == before) {
                    const double cap = std::min(water + settings.bankRise, heightCap);
                    const double amount = settings.depositHeight * across * endWeight;
                    deposit = static_cast<float>(std::max(static_cast<double>(before), (std::min)(cap, oldWorld + amount) / heightScale - point.y / heightScale));
                }
                if (height >= before && deposit <= before && bankWeight < 1.0f / 255.0f) continue;
                const bool protectedVertex = (!includeBoundary && (x == 0 || x == terrain.columns - 1 || z == 0 || z == terrain.rows - 1))
                    || terrain.IsHoleCell(x - 1, z - 1) || terrain.IsHoleCell(x, z - 1)
                    || terrain.IsHoleCell(x - 1, z) || terrain.IsHoleCell(x, z)
                    || (settings.protectedLayer >= 0 && terrain.GetLayerWeightAtGrid(x, z, settings.protectedLayer) >= 0.35f);
                if (!protectedVertex && normalized < -1.0) return false;
                const auto [entry, inserted] = targets.try_emplace(index, Target{height, deposit, bankWeight, bankLayer, water - point.y, protectedVertex});
                if (!inserted) {
                    entry->second.cutHeight = (std::min)(entry->second.cutHeight, height);
                    entry->second.depositHeight = (std::max)(entry->second.depositHeight, deposit);
                    if (bankWeight > entry->second.bankWeight) {
                        entry->second.bankWeight = bankWeight;
                        entry->second.bankLayer = bankLayer;
                        entry->second.waterOffset = water - point.y;
                    }
                }
            }
        }
    }
    TerrainRiverCarvePlan plan;
    plan.vertices.reserve(targets.size());
    for (const auto& [index, target] : targets) {
        if (target.protectedVertex) {
            ++plan.protectedVertices;
            if (includeBoundary) plan.protectedIndices.push_back(index);
            continue;
        }
        const float before = terrain.heightData[index];
        const float height = target.cutHeight < before ? target.cutHeight : target.depositHeight;
        if (height != before) {
            const double change = (static_cast<double>(height) - before) * heightScale;
            if (std::abs(change) > std::numeric_limits<float>::max()) return false;
            plan.deepestCut = (std::max)(plan.deepestCut, static_cast<float>(-change));
            plan.highestDeposit = (std::max)(plan.highestDeposit, static_cast<float>(change));
            plan.vertices.push_back({index, height});
        }
        if (target.bankWeight >= 1.0f / 255.0f) {
            const double clearance = std::abs(static_cast<double>(height) * heightScale - target.waterOffset);
            const float elevation = 1.0f - SmoothUnit(static_cast<float>(clearance / (settings.bankRise + 0.5)));
            const auto offset = index * scene::TERRAIN_SPLAT_SLOTS;
            const auto* indices = terrain.splatIndices.data() + offset;
            const auto* weights = terrain.splatWeights.data() + offset;
            const float weight = std::round(target.bankWeight * elevation * 255.0f) / 255.0f;
            if (weight <= scene::terrain_splat::WeightOf(indices, weights, target.bankLayer)) continue;
            TerrainRiverCarvePlan::Material material{offset, {}, {}};
            std::copy_n(indices, scene::TERRAIN_SPLAT_SLOTS, material.indices.begin());
            std::copy_n(weights, scene::TERRAIN_SPLAT_SLOTS, material.weights.begin());
            scene::terrain_splat::SetWeight(material.indices.data(), material.weights.data(), target.bankLayer, weight);
            if (!std::equal(material.indices.begin(), material.indices.end(), indices)
                || !std::equal(material.weights.begin(), material.weights.end(), weights))
                plan.materials.push_back(material);
        }
    }
    out = std::move(plan);
    return true;
}

bool ResolveTerrainAnalysisSurfaces(scene::Scene& world, const scene::GameObject* single, const OpData* tiles,
    std::vector<TerrainAnalysisSurface>& out)
{
    std::vector<TerrainAnalysisSurface> surfaces;
    const auto append = [&](const scene::GameObject* owner) {
        const auto* terrain = owner ? world.GetComponent<scene::TerrainComponent>(owner->GetID()) : nullptr;
        if (!terrain || !terrain->enabled || !owner->activeInHierarchy()
            || std::any_of(surfaces.begin(), surfaces.end(), [&](const auto& item) { return item.terrain == terrain; }))
            return false;
        for (auto* node = owner; node; node = node->GetParent()) {
            const auto& pose = node->transform;
            const auto scale = pose.scale;
            const double rotationLength = pose.rotation.Length();
            if (!Finite(pose.position) || !Finite(scale)
                || std::abs(scale.x) <= 1.0e-6f || std::abs(scale.y) <= 1.0e-6f || std::abs(scale.z) <= 1.0e-6f
                || !std::isfinite(rotationLength) || std::abs(rotationLength - 1.0) > 0.001) return false;
        }
        surfaces.push_back({terrain, &owner->transform});
        return true;
    };
    if (tiles && !tiles->IsNull()) {
        if (!tiles->IsArray() || tiles->AsArray().empty() || tiles->AsArray().size() > 16) return false;
        for (const auto& id : tiles->AsArray())
            if (!id.IsString() || !append(world.FindByGuid(id.AsString()))) return false;
    } else if (!append(single)) {
        return false;
    }
    out = std::move(surfaces);
    return true;
}

bool TraceTerrainShore(std::span<const TerrainAnalysisSurface> surfaces,
    const TerrainShoreSettings& settings, TerrainShoreOutline& out, std::string& diagnostic)
{
    const auto& seed = settings.seed;
    const auto fail = [&](const char* message) { diagnostic = message; return false; };
    TerrainAnalysisGrid grid;
    if (!SampleTerrainAnalysisGrid(surfaces, settings, grid))
        return fail("検索範囲・格子間隔・簡略化許容値を確認してください。各軸は最大256区間です。");
    const int stride = grid.Stride(), root = grid.CenterIndex();
    const auto& ground = grid.ground;
    if (!(ground[root] < seed.y)) return fail("開始点に水位より低い地表がありません。");
    std::vector<bool> wet(ground.size(), false);
    std::vector<int> queue{root};
    wet[root] = true;
    /// @note The two diagonal neighbors follow the same east–south diagonal as the contour triangles.
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const int id = queue[i];
        if (grid.Boundary(id)) return fail("水没域が検索範囲の外へ流出しています。");
        for (const auto& [dx, dz] : TERRAIN_NEIGHBORS) {
            const int next = id + dx + dz * stride;
            if (!std::isfinite(ground[next])) return fail("水没域の隣に穴または地形の欠損があります。");
            if (!wet[next] && ground[next] < seed.y) { wet[next] = true; queue.push_back(next); }
        }
    }

    struct ShoreVertex { math::Vector3 position; std::array<int, 2> links{}; std::size_t degree = 0; };
    std::vector<ShoreVertex> vertices;
    std::map<std::pair<int, int>, int> edgeVertices;
    const auto crossing = [&](int a, int b) {
        if (a > b) std::swap(a, b);
        const auto [entry, inserted] = edgeVertices.try_emplace({a, b}, static_cast<int>(vertices.size()));
        if (inserted) {
            const auto first = grid.Position(a), last = grid.Position(b);
            /// @note A sample exactly at the waterline is dry. Keep coincident crossings on their own edges.
            const double t = std::clamp((static_cast<double>(seed.y) - ground[a]) / (static_cast<double>(ground[b]) - ground[a]), 0.00001, 0.99999);
            vertices.push_back({{std::lerp(first.x, last.x, static_cast<float>(t)), seed.y,
                std::lerp(first.z, last.z, static_cast<float>(t))}});
        }
        return entry->second;
    };
    const auto triangle = [&](const std::array<int, 3>& ids) {
        std::array<int, 2> segment{};
        std::size_t count = 0;
        for (std::size_t edge = 0; edge < ids.size(); ++edge) {
            const int a = ids[edge], b = ids[(edge + 1) % ids.size()];
            if (wet[a] != wet[b]) segment[count++] = crossing(a, b);
        }
        if (!count) return true;
        auto& a = vertices[segment[0]];
        auto& b = vertices[segment[1]];
        if (a.degree >= a.links.size() || b.degree >= b.links.size()) return false;
        a.links[a.degree++] = segment[1]; b.links[b.degree++] = segment[0];
        return true;
    };
    for (int z = 0; z < grid.nz; ++z) {
        for (int x = 0; x < grid.nx; ++x) {
            const int a = z * stride + x;
            if (!triangle({a, a + stride, a + 1}) || !triangle({a + 1, a + stride, a + stride + 1}))
                return fail("岸の輪郭が分岐しています。");
        }
    }
    if (vertices.size() < 3) return fail("閉じた岸の輪郭を見つけられません。");
    if (vertices.size() > 16384) return fail("岸が細かすぎます。格子間隔を広げてください。");
    std::vector<bool> visited(vertices.size(), false);
    std::vector<std::pair<double, std::vector<math::Vector3>>> rings;
    for (int start = 0; start < static_cast<int>(vertices.size()); ++start) {
        if (visited[start]) continue;
        std::vector<math::Vector3> ring;
        int previous = -1, current = start;
        do {
            const auto& vertex = vertices[current];
            if (vertex.degree != 2 || visited[current]) return fail("岸の輪郭を閉じられません。");
            visited[current] = true;
            ring.push_back(vertex.position);
            const int next = vertex.links[0] == previous ? vertex.links[1] : vertex.links[0];
            previous = current;
            current = next;
        } while (current != start);
        double area = 0.0;
        for (std::size_t i = 1; i + 1 < ring.size(); ++i) {
            const auto a = ring[i] - ring.front(), b = ring[i + 1] - ring.front();
            area += static_cast<double>(a.x) * b.z - static_cast<double>(a.z) * b.x;
        }
        auto simplified = SimplifyShore(ring, settings.tolerance);
        if (simplified.size() < 3 || area == 0.0) return fail("制御点が3〜256点に収まるよう簡略化許容値を調整してください。");
        rings.emplace_back(std::abs(area), std::move(simplified));
    }
    const auto outer = std::max_element(rings.begin(), rings.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    TerrainShoreOutline outline;
    outline.outer = std::move(outer->second);
    for (auto it = rings.begin(); it != rings.end(); ++it)
        if (it != outer) outline.islands.push_back(std::move(it->second));
    out = std::move(outline);
    diagnostic.clear();
    return true;
}

namespace {

std::vector<int> AccumulateTerrainDrainage(const TerrainAnalysisGrid& grid, TerrainDrainage& result,
    std::span<const std::uint8_t> outlets = {}, std::span<const double> inflow = {}, double minRiverArea = 0.0)
{
    result.step = (std::min)(grid.stepX, grid.stepZ);
    result.nodes.resize(grid.ground.size());
    std::vector<int> rank(grid.ground.size(), -1), order;
    std::vector<bool> queued(grid.ground.size(), false);
    using QueueItem = std::pair<float, int>;
    std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<>> queue;
    const auto visit = [&](int id, float waterHeight, int downstream) {
        queued[id] = true;
        auto& node = result.nodes[id];
        node.ground = grid.Position(id);
        node.ground.y = grid.ground[id];
        node.waterHeight = waterHeight;
        node.riverHeight = node.ground.y + DRAINAGE_RIVER_SURFACE_CLEARANCE;
        node.area = static_cast<double>(grid.stepX) * grid.stepZ + (inflow.empty() ? 0.0 : inflow[id]);
        node.downstream = downstream;
        queue.emplace(waterHeight, id);
    };
    const auto isOutlet = [&](int id) { return outlets.empty() ? grid.Boundary(id) : outlets[id] != 0; };
    for (int id = 0; id < static_cast<int>(grid.ground.size()); ++id)
        if (isOutlet(id) && std::isfinite(grid.ground[id])) visit(id, grid.ground[id], -1);
    /// @see https://doi.org/10.1016/j.cageo.2013.04.024 Barnes et al., Priority-Flood: priority queue from known outlets.
    while (!queue.empty()) {
        const auto [height, id] = queue.top();
        queue.pop();
        rank[id] = static_cast<int>(order.size());
        order.push_back(id);
        const int x = id % grid.Stride(), z = id / grid.Stride();
        double steepest = 0.0;
        for (const auto& [dx, dz] : TERRAIN_NEIGHBORS) {
            if (x + dx < 0 || x + dx > grid.nx || z + dz < 0 || z + dz > grid.nz) continue;
            const int next = id + dx + dz * grid.Stride();
            if (!std::isfinite(grid.ground[next])) continue;
            if (!queued[next]) visit(next, (std::max)(height, grid.ground[next]), id);
            if (isOutlet(id) || rank[next] < 0) continue;
            const double gradient = (static_cast<double>(height) - result.nodes[next].waterHeight)
                / std::hypot(static_cast<double>(dx) * grid.stepX, static_cast<double>(dz) * grid.stepZ);
            if (gradient > steepest) { steepest = gradient; result.nodes[id].downstream = next; }
        }
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const auto& node = result.nodes[*it];
        if (node.downstream >= 0) {
            auto& downstream = result.nodes[node.downstream];
            downstream.area += node.area;
            /// @note Filling depressions establishes drainage topology only; the authored channel cuts through the outlet instead of floating over the basin.
            if (node.area >= minRiverArea) downstream.riverHeight = (std::min)(downstream.riverHeight, node.riverHeight);
        }
    }
    return order;
}

struct MapSurface {
    const scene::TerrainComponent* terrain = nullptr;
    const scene::Transform* pose = nullptr;
    double east = 0.0;
    double south = 0.0;
};

struct MapDomain {
    double west = std::numeric_limits<double>::infinity(), north = west;
    double east = -west, south = -west, spacing = west;

    double Width() const { return east - west; }
    double Depth() const { return south - north; }
    bool Contains(double x, double z) const { return x >= west && x <= east && z >= north && z <= south; }
};

bool ReadMapSurface(const TerrainAnalysisSurface& source, MapSurface& out)
{
    if (!source.terrain || !source.pose) return false;
    const auto& terrain = *source.terrain;
    const auto& pose = *source.pose;
    const auto& scale = pose.worldScale;
    const auto& rotation = pose.worldRotation;
    if (!Finite(scale) || !Finite(pose.worldPosition) || scale.x <= 0.0f || scale.y <= 0.0f || scale.z <= 0.0f
        || rotation.x != 0.0f || rotation.y != 0.0f || rotation.z != 0.0f || std::abs(rotation.w) != 1.0f
        || terrain.columns < 2 || terrain.columns > 4097 || terrain.rows < 2 || terrain.rows > 4097
        || terrain.heightData.size() != terrain.VertexCount() || !std::isfinite(terrain.cellSize) || terrain.cellSize <= 0.0f
        || !std::isfinite(terrain.maxHeight) || terrain.maxHeight <= 0.0f
        || (!terrain.holeData.empty() && terrain.holeData.size() != terrain.CellCount())) return false;
    const double east = pose.worldPosition.x + static_cast<double>(terrain.columns - 1) * terrain.cellSize * scale.x;
    const double south = pose.worldPosition.z + static_cast<double>(terrain.rows - 1) * terrain.cellSize * scale.z;
    if (east <= pose.worldPosition.x || south <= pose.worldPosition.z
        || std::abs(east) > std::numeric_limits<float>::max() || std::abs(south) > std::numeric_limits<float>::max()) return false;
    out = {&terrain, &pose, east, south};
    return true;
}

bool MapHasGround(const scene::TerrainComponent& terrain, float localX, float localZ)
{
    if (terrain.holeData.empty()) return true;
    const float x = localX / terrain.cellSize, z = localZ / terrain.cellSize;
    const int cellX = std::clamp(static_cast<int>(x), 0, terrain.columns - 2);
    const int cellZ = std::clamp(static_cast<int>(z), 0, terrain.rows - 2);
    return !terrain.IsHoleCell(cellX, cellZ);
}

float SampleMapHeight(std::span<const MapSurface> surfaces, double x, double z)
{
    float height = -std::numeric_limits<float>::infinity();
    for (const auto& surface : surfaces) {
        const auto& pose = *surface.pose;
        const auto& terrain = *surface.terrain;
        if (x < pose.worldPosition.x || x > surface.east || z < pose.worldPosition.z || z > surface.south) continue;
        const float localX = static_cast<float>((x - pose.worldPosition.x) / pose.worldScale.x);
        const float localZ = static_cast<float>((z - pose.worldPosition.z) / pose.worldScale.z);
        if (!MapHasGround(terrain, localX, localZ)) continue;
        const float sample = pose.worldPosition.y + terrain.GetHeightAt(localX, localZ) * pose.worldScale.y;
        if (std::isfinite(sample)) height = (std::max)(height, sample);
    }
    return height;
}

bool LoadMapSurfaces(std::span<const TerrainAnalysisSurface> sources, std::vector<MapSurface>& surfaces, MapDomain& domain)
{
    if (sources.empty() || sources.size() > 16) return false;
    for (const auto& source : sources) {
        MapSurface surface;
        if (!ReadMapSurface(source, surface)) return false;
        surfaces.push_back(surface);
        const auto& pose = *surface.pose;
        domain.west = (std::min)(domain.west, static_cast<double>(pose.worldPosition.x));
        domain.north = (std::min)(domain.north, static_cast<double>(pose.worldPosition.z));
        domain.east = (std::max)(domain.east, surface.east);
        domain.south = (std::max)(domain.south, surface.south);
        domain.spacing = (std::min)(domain.spacing, static_cast<double>(surface.terrain->cellSize)
            * (std::min)(pose.worldScale.x, pose.worldScale.z));
    }
    return domain.Width() <= 1.0e7 && domain.Depth() <= 1.0e7;
}

bool SampleMapGrid(std::span<const MapSurface> surfaces, const MapDomain& domain, int limit, TerrainAnalysisGrid& grid)
{
    grid.nx = static_cast<int>(std::clamp(std::ceil(domain.Width() / domain.spacing), 2.0, static_cast<double>(limit)));
    grid.nz = static_cast<int>(std::clamp(std::ceil(domain.Depth() / domain.spacing), 2.0, static_cast<double>(limit)));
    grid.stepX = static_cast<float>(domain.Width() / grid.nx);
    grid.stepZ = static_cast<float>(domain.Depth() / grid.nz);
    grid.center = {static_cast<float>(std::midpoint(domain.west, domain.east)), 0.0f,
        static_cast<float>(std::midpoint(domain.north, domain.south))};
    if (grid.stepX <= 0.0f || grid.stepZ <= 0.0f || grid.Position(0).x >= grid.Position(1).x
        || grid.Position(0).z >= grid.Position(grid.Stride()).z) return false;
    grid.ground.reserve(static_cast<std::size_t>(grid.Stride()) * (grid.nz + 1));
    for (int z = 0; z <= grid.nz; ++z) {
        for (int x = 0; x <= grid.nx; ++x) {
            grid.ground.push_back(SampleMapHeight(surfaces, std::lerp(domain.west, domain.east, static_cast<double>(x) / grid.nx),
                std::lerp(domain.north, domain.south, static_cast<double>(z) / grid.nz)));
        }
    }
    return true;
}

struct MapFlow {
    MapDomain domain;
    TerrainAnalysisGrid grid;
    TerrainDrainage drainage;
};

bool BuildMapFlow(std::span<const MapSurface> surfaces, const TerrainFlowRegion& region, MapFlow& coarse, MapFlow& fine,
    double minRiverArea = std::numeric_limits<double>::infinity())
{
    if (!std::isfinite(region.size) || region.size < 0.0f || !Finite(region.center)) return false;
    if (!SampleMapGrid(surfaces, coarse.domain, 256, coarse.grid)) return false;
    const auto order = AccumulateTerrainDrainage(coarse.grid, coarse.drainage, {}, {}, minRiverArea);
    if (region.size == 0.0f) return true;

    const auto& domain = coarse.domain;
    const auto& grid = coarse.grid;
    const auto indexAt = [](double point, double origin, double extent, int count, bool upper) {
        const double index = (point - origin) / extent * count;
        return static_cast<int>(std::clamp(upper ? std::ceil(index) : std::floor(index), 0.0, static_cast<double>(count)));
    };
    const double radius = region.size * 0.5;
    const int left = indexAt(region.center.x - radius, domain.west, domain.Width(), grid.nx, false);
    const int right = indexAt(region.center.x + radius, domain.west, domain.Width(), grid.nx, true);
    const int top = indexAt(region.center.z - radius, domain.north, domain.Depth(), grid.nz, false);
    const int bottom = indexAt(region.center.z + radius, domain.north, domain.Depth(), grid.nz, true);
    if (left >= right || top >= bottom) return false;
    fine.domain = {std::lerp(domain.west, domain.east, static_cast<double>(left) / grid.nx),
        std::lerp(domain.north, domain.south, static_cast<double>(top) / grid.nz),
        std::lerp(domain.west, domain.east, static_cast<double>(right) / grid.nx),
        std::lerp(domain.north, domain.south, static_cast<double>(bottom) / grid.nz), domain.spacing};
    if (!SampleMapGrid(surfaces, fine.domain, 512, fine.grid)) return false;

    const auto contains = [&](int id) {
        const int x = id % grid.Stride(), z = id / grid.Stride();
        return x >= left && x <= right && z >= top && z <= bottom;
    };
    const auto fineId = [&](int id) {
        const int x = id % grid.Stride(), z = id / grid.Stride();
        const int fx = static_cast<int>(std::lround(static_cast<double>(x - left) * fine.grid.nx / (right - left)));
        const int fz = static_cast<int>(std::lround(static_cast<double>(z - top) * fine.grid.nz / (bottom - top)));
        return fz * fine.grid.Stride() + fx;
    };
    std::vector<double> outside(grid.ground.size());
    std::vector<double> inflow(fine.grid.ground.size());
    std::vector<std::uint8_t> outlets(fine.grid.ground.size());
    const double area = static_cast<double>(grid.stepX) * grid.stepZ;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const int id = *it, next = coarse.drainage.nodes[id].downstream;
        const bool inside = contains(id);
        const bool nextInside = next >= 0 && contains(next);
        if (inside && !nextInside) {
            outlets[fineId(id)] = 1;
        } else if (!inside && next >= 0) {
            const double externalArea = outside[id] + area;
            if (nextInside) inflow[fineId(next)] += externalArea;
            else outside[next] += externalArea;
        }
    }
    if (AccumulateTerrainDrainage(fine.grid, fine.drainage, outlets, inflow, minRiverArea).empty()) fine.drainage.nodes.clear();
    return true;
}

TerrainMapRaster MapFlowRaster(const MapFlow& flow, float fullArea)
{
    if (flow.drainage.nodes.empty()) return {};
    const double cellArea = static_cast<double>(flow.grid.stepX) * flow.grid.stepZ;
    const double limit = std::log1p(static_cast<double>(fullArea));
    TerrainMapRaster raster{flow.grid.Stride(), flow.grid.nz + 1, {}};
    raster.pixels.reserve(flow.drainage.nodes.size());
    for (const auto& node : flow.drainage.nodes) {
        raster.pixels.push_back(static_cast<float>(std::clamp(std::log1p((std::max)(0.0, node.area - cellArea)) / limit, 0.0, 1.0)));
    }
    return raster;
}

math::Vector2 SampleMapRelief(std::span<const MapSurface> surfaces, double x, double z, float height, double radius)
{
    constexpr double DIAGONAL = 0.7071067811865475;
    constexpr std::array<std::pair<double, double>, 4> DIRECTIONS{{{1, 0}, {0, 1}, {DIAGONAL, DIAGONAL}, {DIAGONAL, -DIAGONAL}}};
    double mean = 0.0, squares = 0.0;
    int count = 0;
    for (const auto& [dx, dz] : DIRECTIONS) {
        const float a = SampleMapHeight(surfaces, x - dx * radius, z - dz * radius);
        const float b = SampleMapHeight(surfaces, x + dx * radius, z + dz * radius);
        if (!std::isfinite(a) || !std::isfinite(b)) continue;
        const double residual = (std::midpoint(static_cast<double>(a), static_cast<double>(b)) - height) / (radius * 0.2);
        mean += residual;
        squares += residual * residual;
        ++count;
    }
    if (count == 0) return {0.5f, 0.0f};
    return {static_cast<float>(0.5 + 0.5 * std::clamp(mean / count, -1.0, 1.0)),
        static_cast<float>(std::clamp(std::sqrt(squares / count), 0.0, 1.0))};
}

} /// @note namespace

float TerrainMapRaster::Sample(float u, float v) const
{
    const float x = std::clamp(u, 0.0f, 1.0f) * (width - 1), z = std::clamp(v, 0.0f, 1.0f) * (height - 1);
    const int ix = (std::min)(static_cast<int>(x), width - 2), iz = (std::min)(static_cast<int>(z), height - 2);
    const auto offset = static_cast<std::size_t>(iz) * width + ix;
    return std::lerp(std::lerp(pixels[offset], pixels[offset + 1], x - ix),
        std::lerp(pixels[offset + width], pixels[offset + width + 1], x - ix), z - iz);
}

namespace {

using MapValues = std::array<float, 5>;
using MapChannels = std::array<bool, 5>;

bool ValidPreferences(const MapValues& values)
{
    return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v) && std::abs(v) <= 1.0f; });
}

float MatchMapPreferences(const MapValues& channels, const MapValues& preferences)
{
    float value = 1.0f;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        const float preference = preferences[i];
        value *= std::lerp(1.0f, preference >= 0.0f ? channels[i] : 1.0f - channels[i], std::abs(preference));
    }
    return value;
}

template<class OnSample>
bool VisitTerrainMapSamples(std::span<const TerrainAnalysisSurface> sources, const TerrainMapSettings& settings,
    const MapChannels& used, int width, int height, OnSample&& onSample)
{
    if (settings.resolution < 2 || settings.resolution > 513 || !std::isfinite(settings.radius) || settings.radius <= 0.0f
        || !std::isfinite(settings.flowFullArea) || settings.flowFullArea <= 0.0f) return false;
    std::vector<MapSurface> surfaces;
    MapFlow coarse, fine;
    if (!LoadMapSurfaces(sources, surfaces, coarse.domain)) return false;
    if (used[3]) {
        if (!BuildMapFlow(surfaces, settings.flowRegion, coarse, fine)) return false;
    } else if (used[0] && !SampleMapGrid(surfaces, coarse.domain, 256, coarse.grid)) return false;

    float minHeight = std::numeric_limits<float>::infinity(), maxHeight = -minHeight;
    for (const float ground : coarse.grid.ground) {
        if (!std::isfinite(ground)) continue;
        minHeight = (std::min)(minHeight, ground);
        maxHeight = (std::max)(maxHeight, ground);
    }
    const auto flow = MapFlowRaster(coarse, settings.flowFullArea);
    const auto detail = MapFlowRaster(fine, settings.flowFullArea);
    const auto sampleFlow = [](const TerrainMapRaster& raster, const MapDomain& domain, double x, double z) {
        return raster.Sample(static_cast<float>((x - domain.west) / domain.Width()), static_cast<float>((z - domain.north) / domain.Depth()));
    };

    const auto& target = surfaces.front();
    const auto& terrain = *target.terrain;
    const auto& pose = *target.pose;
    const double heightRange = static_cast<double>(maxHeight) - minHeight;
    for (int z = 0; z < height; ++z) {
        const double worldZ = std::lerp(static_cast<double>(pose.worldPosition.z), target.south, static_cast<double>(z) / (height - 1));
        const float localZ = static_cast<float>((worldZ - pose.worldPosition.z) / pose.worldScale.z);
        for (int x = 0; x < width; ++x) {
            const double worldX = std::lerp(static_cast<double>(pose.worldPosition.x), target.east, static_cast<double>(x) / (width - 1));
            const float localX = static_cast<float>((worldX - pose.worldPosition.x) / pose.worldScale.x);
            if (!MapHasGround(terrain, localX, localZ)) continue;
            const float ground = pose.worldPosition.y + terrain.GetHeightAt(localX, localZ) * pose.worldScale.y;
            if (!std::isfinite(ground)) return false;
            MapValues channels{};
            if (used[0] && heightRange > 0.0)
                channels[0] = static_cast<float>(std::clamp((ground - static_cast<double>(minHeight)) / heightRange, 0.0, 1.0));
            if (used[1]) {
                math::Vector3 normal;
                if (!scene::TryTerrainNormalToWorld(pose, terrain.GetNormalAt(localX, localZ), normal)) return false;
                channels[1] = std::acos(std::clamp(normal.y, 0.0f, 1.0f)) * (2.0f / std::numbers::pi_v<float>);
            }
            if (used[2] || used[4]) {
                const auto relief = SampleMapRelief(surfaces, worldX, worldZ, ground, settings.radius);
                channels[2] = relief.x;
                channels[4] = relief.y;
            }
            if (used[3]) {
                channels[3] = sampleFlow(flow, coarse.domain, worldX, worldZ);
                if (!detail.pixels.empty() && fine.domain.Contains(worldX, worldZ)) {
                    const auto& bounds = fine.domain;
                    const double edge = (std::min)({worldX - bounds.west, bounds.east - worldX, worldZ - bounds.north, bounds.south - worldZ});
                    const double fade = (std::min)({static_cast<double>((std::max)(coarse.grid.stepX, coarse.grid.stepZ)),
                        bounds.Width() * 0.25, bounds.Depth() * 0.25});
                    channels[3] = std::lerp(channels[3], sampleFlow(detail, bounds, worldX, worldZ), SmoothUnit(static_cast<float>(edge / fade)));
                }
            }
            onSample(static_cast<std::size_t>(z) * width + x, channels, math::Vector3{static_cast<float>(worldX), ground, static_cast<float>(worldZ)});
        }
    }
    return true;
}

TerrainMapRaster EmptyMapRaster(const scene::TerrainComponent& terrain, int resolution)
{
    const int width = std::clamp(terrain.columns, 2, resolution), height = std::clamp(terrain.rows, 2, resolution);
    return {width, height, std::vector<float>(static_cast<std::size_t>(width) * height)};
}

bool VertexTouchesHole(const scene::TerrainComponent& terrain, int x, int z)
{
    return terrain.IsHoleCell(x - 1, z - 1) || terrain.IsHoleCell(x, z - 1)
        || terrain.IsHoleCell(x - 1, z) || terrain.IsHoleCell(x, z);
}

} /// @note namespace

bool BuildTerrainMap(std::span<const TerrainAnalysisSurface> sources, const TerrainMapSettings& settings,
    TerrainMapChannel channel, TerrainMapRaster& out, const std::function<float(const math::Vector3&)>& habitat)
{
    const auto index = static_cast<std::size_t>(channel);
    if (sources.empty() || !sources.front().terrain || index >= TERRAIN_MAP_NAMES.size() || settings.resolution < 2
        || settings.resolution > 513 || !std::isfinite(settings.density) || settings.density < 0.0f || settings.density > 1.0f
        || !ValidPreferences(settings.preferences)) return false;
    const bool density = channel == TerrainMapChannel::DENSITY;
    MapChannels used{};
    for (std::size_t i = 0; i < used.size(); ++i) used[i] = density ? settings.preferences[i] != 0.0f : i == index;
    auto raster = EmptyMapRaster(*sources.front().terrain, settings.resolution);
    if (!VisitTerrainMapSamples(sources, settings, used, raster.width, raster.height,
        [&](std::size_t pixel, const MapValues& values, const math::Vector3& p) {
            raster.pixels[pixel] = density ? settings.density * MatchMapPreferences(values, settings.preferences)
                * (habitat ? habitat(p) : 1.0f) : values[index];
        })) return false;
    out = std::move(raster);
    return true;
}

void BlendTerrainMap(scene::TerrainComponent& terrain, const TerrainMapRaster& raster, int layer, float strength)
{
    if (strength == 0.0f) return;
    bool ready = terrain.HasValidSplat();
    for (int z = 0; z < terrain.rows; ++z) {
        const float v = static_cast<float>(z) / (terrain.rows - 1);
        for (int x = 0; x < terrain.columns; ++x) {
            if (terrain.IsHoleCell(x - 1, z - 1) || terrain.IsHoleCell(x, z - 1)
                || terrain.IsHoleCell(x - 1, z) || terrain.IsHoleCell(x, z)) continue;
            const float coverage = raster.Sample(static_cast<float>(x) / (terrain.columns - 1), v) * strength;
            if (coverage <= 0.0f) continue;
            if (!ready) {
                terrain.EnsureSplat();
                ready = true;
            }
            const auto offset = (static_cast<std::size_t>(z) * terrain.columns + x) * scene::TERRAIN_SPLAT_SLOTS;
            scene::terrain_splat::BlendToward(terrain.splatIndices.data() + offset, terrain.splatWeights.data() + offset, layer, coverage);
        }
    }
}

bool BakeTerrainMaterials(scene::TerrainComponent& terrain, std::span<const TerrainAnalysisSurface> sources,
    const TerrainMapSettings& maps, const TerrainMaterialBakeSettings& settings)
{
    constexpr std::size_t MAX_RULES = 8;
    if (sources.empty() || !sources.front().terrain || terrain.columns != sources.front().terrain->columns
        || terrain.rows != sources.front().terrain->rows || !terrain.HasValidSplat() || maps.resolution < 2 || maps.resolution > 513
        || settings.rules.empty() || settings.rules.size() > MAX_RULES || settings.protectedLayers.size() > MAX_RULES
        || !std::isfinite(settings.strength) || settings.strength < 0.0f || settings.strength > 1.0f
        || !std::isfinite(settings.protectedThreshold) || settings.protectedThreshold <= 0.0f || settings.protectedThreshold > 1.0f) return false;
    const auto validLayer = [&](int layer) { return layer >= 0 && layer < terrain.LayerCount(); };
    if (!std::all_of(settings.protectedLayers.begin(), settings.protectedLayers.end(), validLayer)) return false;
    if (settings.protection && (settings.protection->width < 2 || settings.protection->height < 2
        || settings.protection->pixels.size() != static_cast<std::size_t>(settings.protection->width) * settings.protection->height)) return false;

    std::array<TerrainMapRaster, MAX_RULES> coverage;
    std::array<int, MAX_RULES + scene::TERRAIN_SPLAT_SLOTS> layers{};
    MapChannels used{};
    const auto count = settings.rules.size();
    for (std::size_t i = 0; i < count; ++i) {
        const auto& rule = settings.rules[i];
        if (!validLayer(rule.layer) || !std::isfinite(rule.weight) || rule.weight < 0.0f || rule.weight > 16.0f
            || !ValidPreferences(rule.preferences)) return false;
        layers[i] = rule.layer;
        coverage[i] = EmptyMapRaster(terrain, maps.resolution);
        for (std::size_t channel = 0; channel < used.size(); ++channel) used[channel] |= rule.preferences[channel] != 0.0f;
    }
    if (settings.strength == 0.0f) return true;
    if (!VisitTerrainMapSamples(sources, maps, used, coverage.front().width, coverage.front().height,
        [&](std::size_t pixel, const MapValues& values, const math::Vector3&) {
            for (std::size_t i = 0; i < count; ++i)
                coverage[i].pixels[pixel] = settings.rules[i].weight * MatchMapPreferences(values, settings.rules[i].preferences);
        })) return false;

    for (int z = 0; z < terrain.rows; ++z) {
        const float v = static_cast<float>(z) / (terrain.rows - 1);
        for (int x = 0; x < terrain.columns; ++x) {
            if (VertexTouchesHole(terrain, x, z)) continue;
            float protectedWeight = 0.0f;
            for (int layer : settings.protectedLayers) protectedWeight += terrain.GetLayerWeightAtGrid(x, z, layer);
            if (protectedWeight >= settings.protectedThreshold) continue;
            const float u = static_cast<float>(x) / (terrain.columns - 1);
            const float strength = settings.strength * (settings.protection ? 1.0f - settings.protection->Sample(u, v) : 1.0f);
            if (strength <= 0.0f) continue;
            std::array<float, MAX_RULES + scene::TERRAIN_SPLAT_SLOTS> weights{};
            float total = 0.0f;
            for (std::size_t i = 0; i < count; ++i) {
                weights[i] = coverage[i].Sample(u, v);
                total += weights[i];
            }
            if (total <= 0.0f) continue;
            for (std::size_t i = 0; i < count; ++i) weights[i] = weights[i] / total * strength;
            const auto offset = (static_cast<std::size_t>(z) * terrain.columns + x) * scene::TERRAIN_SPLAT_SLOTS;
            auto* indices = terrain.splatIndices.data() + offset;
            auto* oldWeights = terrain.splatWeights.data() + offset;
            for (int slot = 0; slot < scene::TERRAIN_SPLAT_SLOTS; ++slot) {
                layers[count + slot] = indices[slot];
                weights[count + slot] = oldWeights[slot] / 255.0f * (1.0f - strength);
            }
            scene::terrain_splat::Canonicalize(layers.data(), weights.data(), static_cast<int>(count) + scene::TERRAIN_SPLAT_SLOTS,
                indices, oldWeights);
        }
    }
    return true;
}

namespace {

std::vector<std::optional<float>> FindLakeFedNodes(const MapFlow& flow,
    std::span<int> next, std::span<const scene::WaterSplineSnapshot> lakes,
    std::vector<std::optional<float>>& outletLevels)
{
    const auto& grid = flow.grid;
    const auto& nodes = flow.drainage.nodes;
    std::vector<std::optional<float>> sourceLevels(nodes.size());
    if (lakes.empty()) return sourceLevels;
    std::vector<bool> submerged(nodes.size());
    std::vector<std::uint8_t> wetState(nodes.size());
    for (const auto& lake : lakes) {
        if (lake.shape != scene::WaterSurfaceShape::SPLINE_LAKE) continue;
        const auto gridIndex = [](float position, float origin, float step, int count) {
            return static_cast<int>(std::clamp(std::floor((position - origin) / step), 0.0f, static_cast<float>(count)));
        };
        const auto origin = grid.Position(0);
        const int x0 = gridIndex(lake.worldBoundsMin.x, origin.x, grid.stepX, grid.nx);
        const int x1 = gridIndex(lake.worldBoundsMax.x, origin.x, grid.stepX, grid.nx);
        const int z0 = gridIndex(lake.worldBoundsMin.z, origin.z, grid.stepZ, grid.nz);
        const int z1 = gridIndex(lake.worldBoundsMax.z, origin.z, grid.stepZ, grid.nz);
        constexpr float SHORE_TOLERANCE = 0.25f;
        constexpr float MIN_LAKE_DEPTH = 0.5f;
        const float lakeLevel = lake.worldBoundsMin.y;
        std::vector<int> wetNodes, connectedBasin;
        for (int z = z0; z <= z1; ++z) for (int x = x0; x <= x1; ++x) {
            const int id = x + z * grid.Stride();
            const auto& node = nodes[id];
            scene::WaterSplineSample wet;
            if (node.area == 0.0 || !scene::TrySampleWaterSpline(lake, node.ground.x, node.ground.z, wet)
                || wet.depth <= 0.0f || node.ground.y >= wet.baseHeight) continue;
            wetNodes.push_back(id);
            wetState[id] = 1;
            if ((std::min)(wet.depth, wet.baseHeight - node.ground.y) >= MIN_LAKE_DEPTH) {
                wetState[id] = 2;
                connectedBasin.push_back(id);
            }
        }
        int outlet = -1;
        for (std::size_t cursor = 0; cursor < connectedBasin.size(); ++cursor) {
            const int id = connectedBasin[cursor];
            const int x = id % grid.Stride(), z = id / grid.Stride();
            for (const auto& [dx, dz] : TERRAIN_NEIGHBORS) {
                if (x + dx < x0 || x + dx > x1 || z + dz < z0 || z + dz > z1) continue;
                const int neighbor = id + dx + dz * grid.Stride();
                if (wetState[neighbor] != 1) continue;
                wetState[neighbor] = 2;
                connectedBasin.push_back(neighbor);
            }
            const auto& node = nodes[id];
            const int receiver = next[id];
            if (receiver < 0 || node.waterHeight > lakeLevel + SHORE_TOLERANCE
                || nodes[receiver].ground.y > lakeLevel + SHORE_TOLERANCE
                || nodes[receiver].riverHeight > lakeLevel) continue;
            scene::WaterSplineSample other;
            if (scene::TrySampleWaterSpline(lake, nodes[receiver].ground.x, nodes[receiver].ground.z, other)) continue;
            if (outlet < 0 || node.waterHeight < nodes[outlet].waterHeight
                || (node.waterHeight == nodes[outlet].waterHeight && node.area > nodes[outlet].area)) outlet = id;
        }
        for (const int id : wetNodes) wetState[id] = 0;
        if (outlet < 0) continue;
        outletLevels[outlet] = lakeLevel;
        for (const int id : connectedBasin) submerged[id] = true;
    }
    for (std::size_t id = 0; id < nodes.size(); ++id) {
        if (submerged[id] && !outletLevels[id]) next[id] = -1;
    }
    for (std::size_t outlet = 0; outlet < nodes.size(); ++outlet) {
        if (!outletLevels[outlet]) continue;
        const float lakeLevel = *outletLevels[outlet];
        /// @note Follow the selected drainage edges through confluences without admitting nearby, disconnected reaches.
        for (int id = next[outlet]; id >= 0; id = next[id]) {
            auto& source = sourceLevels[id];
            if (source && *source >= lakeLevel) break;
            source = lakeLevel;
        }
    }
    return sourceLevels;
}

bool BuildTerrainFlowSections(std::span<const MapSurface> surfaces, MapFlow& coarse, const TerrainFlowCarveSettings& settings,
    bool wholeNetwork, std::vector<TerrainRiverCarveSection>& sections, float& spacing, int& reaches, std::string& diagnostic,
    std::span<const scene::WaterSplineSnapshot> sourceLakes = {}, int* sourceOutletCount = nullptr)
{
    constexpr int MAX_EDGES = 2048;
    const auto fail = [&](const char* reason) { diagnostic = reason; return false; };
    if (!std::isfinite(settings.minArea) || settings.minArea <= 0.0f || !std::isfinite(settings.depthScale)
        || settings.depthScale <= 0.0f || settings.depthScale > 4.0f) return fail("Invalid drainage threshold or depth scale.");
    MapFlow fine;
    if (!BuildMapFlow(surfaces, settings.region, coarse, fine, settings.minArea))
        return fail("Select a valid flow region on positive world-aligned terrain.");
    const auto& flow = settings.region.size > 0.0f ? fine : coarse;
    const auto& nodes = flow.drainage.nodes;
    const auto& target = surfaces.front();
    std::vector<int> next(nodes.size(), -1), incoming(nodes.size());
    int edgeCount = 0;
    const float margin = 24.0f + settings.bed.bankWidth;
    for (int id = 0; id < static_cast<int>(nodes.size()); ++id) {
        const auto& a = nodes[id];
        if (a.downstream < 0 || a.area < settings.minArea) continue;
        const auto& b = nodes[a.downstream];
        if (!wholeNetwork && ((std::max)(a.ground.x, b.ground.x) < target.pose->worldPosition.x - margin
            || (std::min)(a.ground.x, b.ground.x) > target.east + margin
            || (std::max)(a.ground.z, b.ground.z) < target.pose->worldPosition.z - margin
            || (std::min)(a.ground.z, b.ground.z) > target.south + margin)) continue;
        if (++edgeCount > MAX_EDGES) return fail("Too many channel edges. Increase the minimum catchment area or shrink the region.");
        next[id] = a.downstream;
    }

    std::vector<std::optional<float>> outletLevels(nodes.size());
    const auto sourceLevels = FindLakeFedNodes(flow, next, sourceLakes, outletLevels);
    if (sourceOutletCount) *sourceOutletCount = static_cast<int>(std::count_if(outletLevels.begin(), outletLevels.end(),
        [](const auto& level) { return level.has_value(); }));
    for (const int receiver : next)
        if (receiver >= 0) ++incoming[receiver];
    int count = 0;
    const auto append = [&](int id, bool beginsReach) {
        const auto& node = nodes[id];
        const double root = std::sqrt(node.area);
        const float halfWidth = static_cast<float>(std::clamp(0.02 * root, 0.5, 24.0));
        const float depth = static_cast<float>(std::min(4.0, 0.15 + 0.003 * root)) * settings.depthScale;
        const float waterHeight = outletLevels[id].value_or(node.riverHeight);
        sections.push_back({{node.ground.x, node.ground.z}, waterHeight, halfWidth, depth,
            beginsReach, false, false, node.ground.y, sourceLevels[id]});
    };
    for (int id = 0; id < static_cast<int>(nodes.size()); ++id) {
        if (next[id] < 0 || incoming[id] == 1) continue;
        const auto first = sections.size();
        append(id, true);
        sections[first].openStart = incoming[id] > 1;
        int end = next[id];
        for (;;) {
            append(end, false);
            if (next[end] < 0 || incoming[end] != 1) break;
            end = next[end];
        }
        sections[first].openEnd = next[end] >= 0;
        ++count;
    }
    spacing = (std::max)(flow.grid.stepX, flow.grid.stepZ);
    reaches = count;
    diagnostic.clear();
    return true;
}

struct NetworkTile {
    const MapSurface* surface;
    TerrainRiverCarveSettings bed;
    std::map<std::size_t, float> heights;
    std::set<std::size_t> visitedBorders;

    bool Border(int x, int z) const
    {
        return x == 0 || z == 0 || x == surface->terrain->columns - 1 || z == surface->terrain->rows - 1;
    }
    bool Border(std::size_t index) const
    {
        return Border(static_cast<int>(index % surface->terrain->columns), static_cast<int>(index / surface->terrain->columns));
    }
    double HeightScale() const { return static_cast<double>(surface->terrain->maxHeight) * surface->pose->worldScale.y; }
    double WorldHeight(float height) const { return surface->pose->worldPosition.y + height * HeightScale(); }
};

constexpr double NETWORK_SEAM_TOLERANCE = 0.001;

bool MatchNetworkBorder(const NetworkTile& tile, double x, double z, std::optional<std::size_t>& index)
{
    const auto& terrain = *tile.surface->terrain;
    const auto& pose = *tile.surface->pose;
    if (x < pose.worldPosition.x - NETWORK_SEAM_TOLERANCE || x > tile.surface->east + NETWORK_SEAM_TOLERANCE
        || z < pose.worldPosition.z - NETWORK_SEAM_TOLERANCE || z > tile.surface->south + NETWORK_SEAM_TOLERANCE) return true;
    if (std::abs(x - pose.worldPosition.x) > NETWORK_SEAM_TOLERANCE && std::abs(x - tile.surface->east) > NETWORK_SEAM_TOLERANCE
        && std::abs(z - pose.worldPosition.z) > NETWORK_SEAM_TOLERANCE && std::abs(z - tile.surface->south) > NETWORK_SEAM_TOLERANCE) return true;
    const double stepX = static_cast<double>(terrain.cellSize) * pose.worldScale.x;
    const double stepZ = static_cast<double>(terrain.cellSize) * pose.worldScale.z;
    const double fx = (x - pose.worldPosition.x) / stepX, fz = (z - pose.worldPosition.z) / stepZ;
    const auto gx = std::clamp(std::round(fx), 0.0, static_cast<double>(terrain.columns - 1));
    const auto gz = std::clamp(std::round(fz), 0.0, static_cast<double>(terrain.rows - 1));
    if (std::abs(fx - gx) * stepX > NETWORK_SEAM_TOLERANCE || std::abs(fz - gz) * stepZ > NETWORK_SEAM_TOLERANCE) return false;
    index = static_cast<std::size_t>(gz) * terrain.columns + static_cast<std::size_t>(gx);
    return true;
}

bool SameNetworkEdgeSpacing(const MapSurface& a, const MapSurface& b)
{
    const auto& pa = a.pose->worldPosition;
    const auto& pb = b.pose->worldPosition;
    const auto near = [](double x, double y) { return std::abs(x - y) <= NETWORK_SEAM_TOLERANCE; };
    if ((near(pa.x, b.east) || near(a.east, pb.x))
        && (std::min)(a.south, b.south) - (std::max)(pa.z, pb.z) > NETWORK_SEAM_TOLERANCE
        && !near(static_cast<double>(a.terrain->cellSize) * a.pose->worldScale.z,
            static_cast<double>(b.terrain->cellSize) * b.pose->worldScale.z)) return false;
    if ((near(pa.z, b.south) || near(a.south, pb.z))
        && (std::min)(a.east, b.east) - (std::max)(pa.x, pb.x) > NETWORK_SEAM_TOLERANCE
        && !near(static_cast<double>(a.terrain->cellSize) * a.pose->worldScale.x,
            static_cast<double>(b.terrain->cellSize) * b.pose->worldScale.x)) return false;
    return true;
}

bool AlignNetworkBorders(std::vector<NetworkTile>& tiles, std::vector<TerrainRiverCarvePlan>& plans, std::string& diagnostic)
{
    for (std::size_t owner = 0; owner < tiles.size(); ++owner) {
        auto& tile = tiles[owner];
        const auto& terrain = *tile.surface->terrain;
        const auto& pose = *tile.surface->pose;
        for (const auto& proposed : plans[owner].vertices) {
            if (!tile.Border(proposed.index) || tile.visitedBorders.contains(proposed.index)) continue;
            const double x = pose.worldPosition.x + static_cast<double>(proposed.index % terrain.columns) * terrain.cellSize * pose.worldScale.x;
            const double z = pose.worldPosition.z + static_cast<double>(proposed.index / terrain.columns) * terrain.cellSize * pose.worldScale.z;
            std::vector<std::pair<std::size_t, std::size_t>> group;
            double lowest = std::numeric_limits<double>::infinity(), highest = -lowest;
            bool cut = false, protectedVertex = false;
            const double initial = tile.WorldHeight(terrain.heightData[proposed.index]);
            for (std::size_t i = 0; i < tiles.size(); ++i) {
                auto& other = tiles[i];
                std::optional<std::size_t> match;
                if (i == owner) match = proposed.index;
                else if (!MatchNetworkBorder(other, x, z, match)) {
                    diagnostic = "The channel crosses a shared edge whose terrain vertices are not aligned.";
                    return false;
                }
                if (!match) continue;
                if (!SameNetworkEdgeSpacing(*tile.surface, *other.surface)) {
                    diagnostic = "Use the same vertex spacing on both sides of the shared river edge.";
                    return false;
                }
                other.visitedBorders.insert(*match);
                group.emplace_back(i, *match);
                const auto& source = *other.surface->terrain;
                const double before = other.WorldHeight(source.heightData[*match]);
                if (!std::isfinite(before) || std::abs(initial - before) > 0.05) {
                    diagnostic = "Align the existing shared-edge heights before carving the network.";
                    return false;
                }
                const auto existing = other.heights.find(*match);
                if (existing != other.heights.end()) {
                    const double height = other.WorldHeight(existing->second);
                    cut |= height < before;
                    lowest = (std::min)(lowest, height);
                    highest = (std::max)(highest, height);
                }
                const int gx = static_cast<int>(*match % source.columns), gz = static_cast<int>(*match / source.columns);
                protectedVertex |= VertexTouchesHole(source, gx, gz) || (other.bed.protectedLayer >= 0
                    && source.GetLayerWeightAtGrid(gx, gz, other.bed.protectedLayer) >= 0.35f);
            }
            const double height = cut ? lowest : highest;
            for (const auto& [i, index] : group) {
                auto& other = tiles[i];
                if (group.size() == 1 || protectedVertex) {
                    other.heights.erase(index);
                    plans[i].protectedIndices.push_back(index);
                    continue;
                }
                const double normalized = (height - other.surface->pose->worldPosition.y) / other.HeightScale();
                if (normalized < -1.0 || normalized > 1.0) {
                    diagnostic = "A reconciled shared-edge height exceeds one tile's height range.";
                    return false;
                }
                other.heights[index] = static_cast<float>(normalized);
            }
        }
    }
    return true;
}

} /// @note namespace

bool PlanTerrainFlowCarve(std::span<const TerrainAnalysisSurface> sources, const TerrainFlowCarveSettings& settings,
    TerrainRiverCarvePlan& out, int& reaches, std::string& diagnostic)
{
    std::vector<MapSurface> surfaces;
    MapFlow coarse;
    if (!LoadMapSurfaces(sources, surfaces, coarse.domain)) { diagnostic = "Select valid terrain tiles."; return false; }
    std::vector<TerrainRiverCarveSection> sections;
    float spacing = 0.0f;
    if (!BuildTerrainFlowSections(surfaces, coarse, settings, false, sections, spacing, reaches, diagnostic)) return false;
    TerrainRiverCarvePlan plan;
    const auto& target = surfaces.front();
    if (!sections.empty() && !PlanTerrainRiverCarve(*target.terrain, *target.pose, sections, settings.bed, plan)) {
        diagnostic = "The river bed settings, terrain height range or vertex budget could not be satisfied.";
        return false;
    }
    plan.analysisSpacing = spacing;
    out = std::move(plan);
    return true;
}

bool PlanTerrainFlowNetwork(std::span<const TerrainAnalysisSurface> sources, const TerrainFlowCarveSettings& settings,
    TerrainFlowNetworkPlan& out, std::string& diagnostic, std::span<const scene::WaterSplineSnapshot> sourceLakes)
{
    std::vector<MapSurface> surfaces;
    MapFlow coarse;
    if (!LoadMapSurfaces(sources, surfaces, coarse.domain)) { diagnostic = "Select valid terrain tiles."; return false; }
    const auto& first = *surfaces.front().terrain;
    for (const int layer : {settings.bed.bankLayer, settings.bed.gentleBankLayer, settings.bed.protectedLayer})
        if (layer < -1 || layer >= first.LayerCount()) { diagnostic = "Choose valid material layers on the selected tile."; return false; }
    for (std::size_t i = 0; i < surfaces.size(); ++i) {
        for (std::size_t j = i + 1; j < surfaces.size(); ++j) {
            const auto& a = surfaces[i];
            const auto& b = surfaces[j];
            if ((std::min)(a.east, b.east) - (std::max)(a.pose->worldPosition.x, b.pose->worldPosition.x) > NETWORK_SEAM_TOLERANCE
                && (std::min)(a.south, b.south) - (std::max)(a.pose->worldPosition.z, b.pose->worldPosition.z) > NETWORK_SEAM_TOLERANCE) {
                diagnostic = "Flow network tiles must not overlap.";
                return false;
            }
        }
    }
    TerrainFlowNetworkPlan plan;
    int reaches = 0;
    if (!BuildTerrainFlowSections(surfaces, coarse, settings, true, plan.sections, plan.analysisSpacing, reaches, diagnostic,
            sourceLakes, &plan.sourceOutletCount)) return false;
    if (reaches > 96) { diagnostic = "Too many river reaches. Increase the catchment threshold or shrink the region."; return false; }
    plan.tiles.resize(surfaces.size());
    std::vector<NetworkTile> tiles;
    for (std::size_t i = 0; i < surfaces.size(); ++i) {
        const auto& surface = surfaces[i];
        const auto layerOnTile = [&](int layer) {
            if (layer < 0 || i == 0) return layer;
            const auto& reference = first.layerMaterials[layer];
            const auto& layers = surface.terrain->layerMaterials;
            const auto found = std::find(layers.begin(), layers.end(), reference);
            return found == layers.end() ? -1 : static_cast<int>(found - layers.begin());
        };
        auto bed = settings.bed;
        bed.endFade = 0.0f;
        bed.bankLayer = layerOnTile(bed.bankLayer);
        bed.gentleBankLayer = layerOnTile(bed.gentleBankLayer);
        bed.protectedLayer = layerOnTile(bed.protectedLayer);
        auto& tile = tiles.emplace_back(NetworkTile{&surface, bed, {}, {}});
        auto& local = plan.tiles[i];
        if (!plan.sections.empty() && !PlanTerrainRiverCarve(*surface.terrain, *surface.pose, plan.sections, bed, local, true)) {
            diagnostic = "A terrain tile exceeds the carve budget or has invalid height or layer data.";
            return false;
        }
        for (const auto& edit : local.vertices) tile.heights.emplace(edit.index, edit.normalizedHeight);
    }
    if (!AlignNetworkBorders(tiles, plan.tiles, diagnostic)) return false;
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const auto& tile = tiles[i];
        auto& local = plan.tiles[i];
        local.vertices.clear();
        local.deepestCut = local.highestDeposit = 0.0f;
        for (const auto& [index, height] : tile.heights) {
            const float before = tile.surface->terrain->heightData[index];
            if (height == before) continue;
            const float change = static_cast<float>((static_cast<double>(height) - before) * tile.HeightScale());
            if (!std::isfinite(change)) { diagnostic = "A proposed height change is out of range."; return false; }
            local.deepestCut = (std::max)(local.deepestCut, -change);
            local.highestDeposit = (std::max)(local.highestDeposit, change);
            local.vertices.push_back({index, height});
        }
        std::erase_if(local.materials, [&](const auto& material) { return tile.Border(material.offset / scene::TERRAIN_SPLAT_SLOTS); });
        std::sort(local.protectedIndices.begin(), local.protectedIndices.end());
        local.protectedIndices.erase(std::unique(local.protectedIndices.begin(), local.protectedIndices.end()), local.protectedIndices.end());
        local.protectedVertices = local.protectedIndices.size();
    }
    out = std::move(plan);
    diagnostic.clear();
    return true;
}

std::vector<std::size_t> FindTerrainWaterfallLips(std::span<const TerrainRiverCarveSection> sections,
    float minDrop, float minGrade)
{
    std::vector<std::size_t> lips;
    if (!std::isfinite(minDrop) || minDrop <= 0.0f || !std::isfinite(minGrade) || minGrade <= 0.0f) return lips;
    for (std::size_t lip = 1; lip + 2 < sections.size(); ++lip) {
        const auto& a = sections[lip];
        const auto& b = sections[lip + 1];
        if (a.beginsReach || b.beginsReach || sections[lip + 2].beginsReach
            || !a.sourceLakeHeight || *a.sourceLakeHeight <= a.waterHeight) continue;
        const double run = std::hypot(static_cast<double>(b.center.x) - a.center.x,
            static_cast<double>(b.center.y) - a.center.y);
        const double drop = static_cast<double>(a.waterHeight) - b.waterHeight;
        const double groundDrop = static_cast<double>(a.groundHeight) - b.groundHeight;
        /// @note A lowered spill saddle is a cut, not an existing cliff lip or plunge point.
        constexpr float MAX_GROUND_ADJUSTMENT = 0.25f;
        if (run > 0.001 && drop >= minDrop && drop >= run * minGrade
            && groundDrop >= minDrop && groundDrop >= run * minGrade
            && std::abs(a.groundHeight - a.waterHeight) <= MAX_GROUND_ADJUSTMENT
            && std::abs(b.groundHeight - b.waterHeight) <= MAX_GROUND_ADJUSTMENT) {
            lips.push_back(lip);
            ++lip;
        }
    }
    return lips;
}

bool BuildTerrainDrainage(std::span<const TerrainAnalysisSurface> surfaces,
    const TerrainShoreSettings& settings, TerrainDrainage& out, std::string& diagnostic, double minRiverArea)
{
    TerrainAnalysisGrid grid;
    if (!SampleTerrainAnalysisGrid(surfaces, settings, grid)) {
        diagnostic = "排水範囲と格子間隔を確認してください。各軸は最大256区間です。";
        return false;
    }
    TerrainDrainage result;
    result.seed = grid.CenterIndex();
    AccumulateTerrainDrainage(grid, result, {}, {}, minRiverArea);
    if (result.nodes[result.seed].area == 0.0) {
        diagnostic = "開始点から検索範囲の有効な外周へ排水経路を求められません。";
        return false;
    }
    result.spill = result.seed;
    for (int id = result.seed; id >= 0; id = result.nodes[id].downstream) {
        if (result.nodes[id].ground.y > result.nodes[result.spill].ground.y) result.spill = id;
        result.outlet = id;
    }
    out = std::move(result);
    diagnostic.clear();
    return true;
}

math::Vector2 DrainageRiverSize(const TerrainDrainageNode& node, float step)
{
    const double root = std::sqrt(node.area);
    const float cap = std::clamp(step * 0.7f, 0.15f, 24.0f);
    const float width = static_cast<float>(std::clamp(0.03 * root, 0.15, static_cast<double>(cap)));
    const float depth = static_cast<float>(std::min(4.0, 0.15 + 0.003 * root));
    return {width, depth};
}

bool TerrainAuthoringValuesEqual(const scene::TerrainComponent& first, const scene::TerrainComponent& second)
{
    if (first.columns != second.columns || first.rows != second.rows || first.cellSize != second.cellSize
        || first.maxHeight != second.maxHeight || first.heightBlendDepth != second.heightBlendDepth
        || first.chunkSize != second.chunkSize || first.enabled != second.enabled
        || first.heightData != second.heightData || first.layerMaterials != second.layerMaterials
        || first.splatIndices != second.splatIndices || first.splatWeights != second.splatWeights
        || first.holeData != second.holeData || first.surfaceMaterial != second.surfaceMaterial
        || first.surfaceIndices != second.surfaceIndices || first.surfacePositions.size() != second.surfacePositions.size()) return false;
    for (size_t i = 0; i < first.surfacePositions.size(); ++i) {
        const auto& a = first.surfacePositions[i];
        const auto& b = second.surfacePositions[i];
        if (a.x != b.x || a.y != b.y || a.z != b.z) return false;
    }
    return true;
}

void ApplyTerrainAuthoringSnapshot(scene::TerrainComponent& terrain, const scene::TerrainComponent& snapshot, bool geometry)
{
    const auto assetPath = terrain.terrainAssetPath;
    const auto surfaceRevision = terrain.surfaceRevision;
    const auto heightPreview = terrain.heightPreview;
    const auto vegetationRevision = terrain.vegetationRevision;
    const bool heightDirty = terrain.heightDirty, colliderDirty = terrain.colliderDirty;
    const bool materialDirty = terrain.materialParamDirty || terrain.layerMaterials != snapshot.layerMaterials
        || terrain.heightBlendDepth != snapshot.heightBlendDepth;
    terrain = snapshot;
    /// @note 保存後の参照と描画側が進めた世代は履歴の値へ巻き戻さない。
    terrain.terrainAssetPath = assetPath;
    terrain.surfaceRevision = surfaceRevision;
    terrain.heightPreview = heightPreview;
    terrain.vegetationRevision = vegetationRevision;
    terrain.heightDirty = heightDirty;
    terrain.colliderDirty = colliderDirty;
    terrain.materialParamDirty = materialDirty;
    terrain.RequestSplatRebuild();
    if (geometry) terrain.RequestHeightRebuild();
}

bool IsValidTerrainHeightStamp(const scene::TerrainComponent& terrain, const TerrainHeightStampSettings& settings)
{
    return terrain.columns >= 2 && terrain.rows >= 2 && terrain.columns <= 4097 && terrain.rows <= 4097
        && terrain.heightData.size() == terrain.VertexCount()
        && std::isfinite(terrain.cellSize) && terrain.cellSize > 0.0f && terrain.cellSize <= 100000.0f
        && std::isfinite(terrain.maxHeight) && terrain.maxHeight > 0.0f
        && std::isfinite(settings.centerLocal.x) && std::isfinite(settings.centerLocal.y) && std::isfinite(settings.centerLocal.z)
        && std::isfinite(settings.width) && settings.width > 0.0f && settings.width <= 100000.0f
        && std::isfinite(settings.depth) && settings.depth > 0.0f && settings.depth <= 100000.0f
        && std::isfinite(settings.rotationDegrees)
        && std::isfinite(settings.heightScale) && std::abs(settings.heightScale) <= 100000.0f
        && std::isfinite(settings.heightOffset) && std::abs(settings.heightOffset) <= 100000.0f
        && std::isfinite(settings.strength) && settings.strength >= 0.0f && settings.strength <= 1.0f
        && std::isfinite(settings.edgeFade) && settings.edgeFade >= 0.0f && settings.edgeFade <= 0.5f
        && settings.blend >= TerrainHeightStampBlend::ADD && settings.blend <= TerrainHeightStampBlend::MINIMUM;
}

bool ApplyTerrainHeightStamp(scene::TerrainComponent& terrain, const scene::TerrainComponent& image,
    const TerrainHeightStampSettings& settings)
{
    if (!IsValidTerrainHeightStamp(terrain, settings) || image.columns < 2 || image.rows < 2
        || image.columns > 4097 || image.rows > 4097 || image.heightData.size() != image.VertexCount()) return false;
    for (float height : terrain.heightData)
        if (!std::isfinite(height)) return false;
    for (float height : image.heightData)
        if (!std::isfinite(height) || height < -1.0f || height > 1.0f) return false;

    constexpr double DEGREES_TO_RADIANS = 0.017453292519943295;
    const double angle = std::remainder(static_cast<double>(settings.rotationDegrees), 360.0) * DEGREES_TO_RADIANS;
    const double cosine = std::cos(angle), sine = std::sin(angle);
    const double halfX = settings.width * 0.5, halfZ = settings.depth * 0.5;
    const double extentX = std::abs(cosine) * halfX + std::abs(sine) * halfZ;
    const double extentZ = std::abs(sine) * halfX + std::abs(cosine) * halfZ;
    const auto gridX = [&](double value) { return static_cast<int>(std::clamp(value / terrain.cellSize, 0.0, static_cast<double>(terrain.columns - 1))); };
    const auto gridZ = [&](double value) { return static_cast<int>(std::clamp(value / terrain.cellSize, 0.0, static_cast<double>(terrain.rows - 1))); };
    const int minX = gridX(settings.centerLocal.x - extentX), maxX = std::min(gridX(settings.centerLocal.x + extentX) + 1, terrain.columns - 1);
    const int minZ = gridZ(settings.centerLocal.z - extentZ), maxZ = std::min(gridZ(settings.centerLocal.z + extentZ) + 1, terrain.rows - 1);
    auto heights = terrain.heightData;
    for (int z = minZ; z <= maxZ; ++z) {
        for (int x = minX; x <= maxX; ++x) {
            const double dx = static_cast<double>(x) * terrain.cellSize - settings.centerLocal.x;
            const double dz = static_cast<double>(z) * terrain.cellSize - settings.centerLocal.z;
            const double rawU = (cosine * dx - sine * dz) / settings.width + 0.5;
            const double rawV = (sine * dx + cosine * dz) / settings.depth + 0.5;
            /// @note 直角回転の丸めで画像端の頂点が落ちるのを防ぎ、読み取り座標は必ず画像内へ戻す。
            constexpr double UV_ROUNDING = 1e-12;
            if (rawU < -UV_ROUNDING || rawU > 1.0 + UV_ROUNDING || rawV < -UV_ROUNDING || rawV > 1.0 + UV_ROUNDING) continue;
            const double u = std::clamp(rawU, 0.0, 1.0), v = std::clamp(rawV, 0.0, 1.0);
            const double imageX = u * (image.columns - 1), imageZ = v * (image.rows - 1);
            const int x0 = static_cast<int>(imageX), z0 = static_cast<int>(imageZ);
            const int x1 = std::min(x0 + 1, image.columns - 1), z1 = std::min(z0 + 1, image.rows - 1);
            const auto sample = [&](int sx, int sz) { return static_cast<double>(image.heightData[static_cast<size_t>(sz) * image.columns + sx]); };
            const double top = std::lerp(sample(x0, z0), sample(x1, z0), imageX - x0);
            const double bottom = std::lerp(sample(x0, z1), sample(x1, z1), imageX - x0);
            const double value = std::lerp(top, bottom, imageZ - z0) * settings.heightScale + settings.heightOffset;
            const size_t index = static_cast<size_t>(z) * terrain.columns + x;
            const double current = static_cast<double>(heights[index]) * terrain.maxHeight;
            double target = value;
            switch (settings.blend) {
                case TerrainHeightStampBlend::ADD: target = current + value; break;
                case TerrainHeightStampBlend::REPLACE: break;
                case TerrainHeightStampBlend::MAXIMUM: target = std::max(current, value); break;
                case TerrainHeightStampBlend::MINIMUM: target = std::min(current, value); break;
            }
            const double edge = std::min({u, 1.0 - u, v, 1.0 - v});
            const double fade = settings.edgeFade > 0.0f ? std::clamp(edge / settings.edgeFade, 0.0, 1.0) : 1.0;
            const double weight = settings.strength * fade * fade * (3.0 - 2.0 * fade);
            if (weight == 0.0) continue;
            heights[index] = static_cast<float>(std::clamp(std::lerp(current, target, weight) / terrain.maxHeight, -1.0, 1.0));
        }
    }
    if (heights != terrain.heightData) {
        terrain.heightData.swap(heights);
        terrain.RequestHeightRebuild();
    }
    return true;
}

bool BakeTerrainBiome(scene::TerrainComponent& terrain, const scene::Transform& transform,
    const TerrainBiomeSettings& settings)
{
    if (terrain.columns < 2 || terrain.rows < 2 || terrain.columns > 4097 || terrain.rows > 4097
        || terrain.heightData.size() != terrain.VertexCount()
        || !std::isfinite(terrain.cellSize) || terrain.cellSize <= 0.0f
        || !std::isfinite(terrain.maxHeight) || terrain.maxHeight <= 0.0f
        || !std::isfinite(settings.snowHeight) || !std::isfinite(settings.snowFade) || settings.snowFade < 0.0f
        || !std::isfinite(settings.rockSlope) || settings.rockSlope < 0.0f || settings.rockSlope > 90.0f
        || !std::isfinite(settings.slopeFade) || settings.slopeFade < 0.0f || settings.slopeFade > 90.0f)
        return false;
    const int layers[] = {settings.grassLayer, settings.rockLayer, settings.snowLayer};
    for (int layer : layers)
        if (layer < 0 || layer >= terrain.LayerCount()) return false;
    if (layers[0] == layers[1] || layers[0] == layers[2] || layers[1] == layers[2]) return false;
    for (float height : terrain.heightData)
        if (!std::isfinite(height) || !std::isfinite(height * terrain.maxHeight)) return false;

    const auto world = transform.GetWorldMatrix();
    std::vector<std::uint8_t> indices(terrain.VertexCount() * scene::TERRAIN_SPLAT_SLOTS);
    std::vector<std::uint8_t> weights(indices.size());
    constexpr float RADIANS_TO_DEGREES = 57.29577951308232f;
    for (int z = 0; z < terrain.rows; ++z) {
        for (int x = 0; x < terrain.columns; ++x) {
            const auto vertex = static_cast<size_t>(z) * terrain.columns + x;
            const auto point = world * math::Vector4{static_cast<float>(x) * terrain.cellSize,
                terrain.heightData[vertex] * terrain.maxHeight, static_cast<float>(z) * terrain.cellSize, 1.0f};
            math::Vector3 normal;
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)
                || !scene::TryTerrainNormalToWorld(transform, terrain.ComputeNormal(x, z), normal)) return false;
            const float slope = std::acos(std::clamp(normal.y, -1.0f, 1.0f)) * RADIANS_TO_DEGREES;
            const float rock = Transition(slope, settings.rockSlope, settings.slopeFade);
            const float snow = (1.0f - rock) * Transition(point.y, settings.snowHeight, settings.snowFade);
            const float distribution[] = {(1.0f - rock) - snow, rock, snow};
            const auto offset = vertex * scene::TERRAIN_SPLAT_SLOTS;
            scene::terrain_splat::Canonicalize(layers, distribution, 3, indices.data() + offset, weights.data() + offset);
        }
    }
    terrain.splatIndices.swap(indices);
    terrain.splatWeights.swap(weights);
    terrain.RequestSplatRebuild();
    return true;
}

} /// @note namespace fbzz::editor
