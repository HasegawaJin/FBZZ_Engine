/// @file    SurfaceFarMesh.cpp
/// @brief   連続面の外周 strip と corner の座標・高さ標本化帯域を生成する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <Graphics/Renderer/SurfaceFarMesh.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace fbzz::renderer {
namespace {

bool Finite(math::Vector2 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool Finite(math::Vector3 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

math::Vector2 Maximum(math::Vector2 a, math::Vector2 b)
{
    return {(std::max)(a.x, b.x), (std::max)(a.y, b.y)};
}

struct Boundary {
    float begin = 0.0f, end = 0.0f;
    size_t leaf = 0;
};

bool Complete(std::vector<Boundary>& boundary)
{
    std::sort(boundary.begin(), boundary.end(), [](const Boundary& a, const Boundary& b) {
        return a.begin < b.begin;
    });
    float end = 0.0f;
    for (const auto& part : boundary) {
        if (part.begin != end || part.end <= part.begin) return false;
        end = part.end;
    }
    return end == 1.0f;
}

bool WorldBoundsFinite(const SurfacePatchSurface& surface, const SurfacePatch& patch)
{
    if (!Finite(patch.localMin) || !Finite(patch.localMax) || !Finite(patch.aabbMin) || !Finite(patch.aabbMax))
        return false;
    const auto center = patch.aabbMin * 0.5f + patch.aabbMax * 0.5f;
    const auto extents = patch.aabbMax * 0.5f - patch.aabbMin * 0.5f;
    const auto position = surface.worldMatrix * math::Vector4{center.x, center.y, center.z, 1.0f};
    const math::Vector3 worldCenter{position.x, position.y, position.z};
    auto extent = [&](int row) {
        return std::abs(surface.worldMatrix.m[row][0]) * extents.x
            + std::abs(surface.worldMatrix.m[row][1]) * extents.y
            + std::abs(surface.worldMatrix.m[row][2]) * extents.z;
    };
    const math::Vector3 worldExtents{extent(0), extent(1), extent(2)};
    return Finite(worldCenter) && Finite(worldExtents)
        && Finite(worldCenter - worldExtents) && Finite(worldCenter + worldExtents);
}

SurfacePatch MakeFarPatch(const SurfacePatchSurface& inner, math::Vector2 uvMin, math::Vector2 uvMax)
{
    SurfacePatch patch;
    patch.uvMin = uvMin;
    patch.uvMax = uvMax;
    const auto extent = inner.localMax - inner.localMin;
    patch.localMin = {inner.localMin.x + extent.x * uvMin.x, inner.localMin.y + extent.y * uvMin.y};
    patch.localMax = {inner.localMin.x + extent.x * uvMax.x, inner.localMin.y + extent.y * uvMax.y};
    patch.aabbMin = {patch.localMin.x - inner.horizontalDisplacement, inner.minHeight,
        patch.localMin.y - inner.horizontalDisplacement};
    patch.aabbMax = {patch.localMax.x + inner.horizontalDisplacement, inner.maxHeight,
        patch.localMax.y + inner.horizontalDisplacement};
    patch.morphCellSize = patch.cellSize;
    return patch;
}

} /// @note namespace

bool BuildSurfaceFarMesh(const SurfacePatchSurface& inner, std::span<const SurfacePatch> innerLeaves,
    math::Vector2 outerMin, math::Vector2 outerMax, uint32_t cellsPerPatch,
    std::vector<SurfaceFarPatch>& out, bool morphEnabled)
{
    const auto extent = inner.localMax - inner.localMin;
    if (!Finite(inner.localMin) || !Finite(inner.localMax) || !Finite(extent)
        || extent.x <= 0.0f || extent.y <= 0.0f || !Finite(outerMin) || !Finite(outerMax)
        || outerMin.x > inner.localMin.x || outerMin.y > inner.localMin.y
        || outerMax.x < inner.localMax.x || outerMax.y < inner.localMax.y
        || !std::isfinite(inner.minHeight) || !std::isfinite(inner.maxHeight) || inner.minHeight > inner.maxHeight
        || !std::isfinite(inner.horizontalDisplacement) || inner.horizontalDisplacement < 0.0f
        || cellsPerPatch < 2 || cellsPerPatch > 256 || (cellsPerPatch & (cellsPerPatch - 1)) != 0
        || innerLeaves.empty() || innerLeaves.size() > 16384)
        return false;
    for (const auto& row : inner.worldMatrix.m)
        for (float value : row)
            if (!std::isfinite(value)) return false;
    if (inner.worldMatrix.m[3][0] != 0.0f || inner.worldMatrix.m[3][1] != 0.0f
        || inner.worldMatrix.m[3][2] != 0.0f || inner.worldMatrix.m[3][3] != 1.0f) return false;
    for (uint32_t column = 0; column < 3; ++column) {
        const math::Vector3 axis{inner.worldMatrix.m[0][column], inner.worldMatrix.m[1][column], inner.worldMatrix.m[2][column]};
        const float lengthSquared = axis.LengthSq();
        if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0f) return false;
    }
    const math::Vector3 axisX{inner.worldMatrix.m[0][0], inner.worldMatrix.m[1][0], inner.worldMatrix.m[2][0]};
    const math::Vector3 axisZ{inner.worldMatrix.m[0][2], inner.worldMatrix.m[1][2], inner.worldMatrix.m[2][2]};
    const math::Vector2 axisScale{axisX.Length(), axisZ.Length()};
    if (!Finite(axisScale) || axisScale.x <= 0.0f || axisScale.y <= 0.0f) return false;
    const math::Vector2 outerUvMin{(outerMin.x - inner.localMin.x) / extent.x,
        (outerMin.y - inner.localMin.y) / extent.y};
    const math::Vector2 outerUvMax{(outerMax.x - inner.localMin.x) / extent.x,
        (outerMax.y - inner.localMin.y) / extent.y};
    if (!Finite(outerUvMin) || !Finite(outerUvMax)) return false;
    std::array<std::vector<Boundary>, 4> boundaries;
    for (size_t index = 0; index < innerLeaves.size(); ++index) {
        const auto& leaf = innerLeaves[index];
        if (!Finite(leaf.uvMin) || !Finite(leaf.uvMax) || !Finite(leaf.cellSize)
            || (morphEnabled && (!Finite(leaf.morphCellSize)
                || leaf.morphCellSize.x <= 0.0f || leaf.morphCellSize.y <= 0.0f))
            || leaf.uvMin.x < 0.0f || leaf.uvMin.y < 0.0f || leaf.uvMax.x > 1.0f || leaf.uvMax.y > 1.0f
            || leaf.uvMin.x >= leaf.uvMax.x || leaf.uvMin.y >= leaf.uvMax.y) return false;
        for (uint32_t boundary = 0; boundary < 4; ++boundary)
            if (!Finite(leaf.edgeCellSize[boundary]) || !Finite(leaf.cornerCellSize[boundary])) return false;
        if (leaf.uvMin.x == 0.0f) boundaries[0].push_back({leaf.uvMin.y, leaf.uvMax.y, index});
        if (leaf.uvMax.x == 1.0f) boundaries[1].push_back({leaf.uvMin.y, leaf.uvMax.y, index});
        if (leaf.uvMin.y == 0.0f) boundaries[2].push_back({leaf.uvMin.x, leaf.uvMax.x, index});
        if (leaf.uvMax.y == 1.0f) boundaries[3].push_back({leaf.uvMin.x, leaf.uvMax.x, index});
    }
    for (auto& boundary : boundaries) if (!Complete(boundary)) return false;
    std::vector<SurfaceFarPatch> result;
    result.reserve(innerLeaves.size() + 4u);
    auto append = [&](SurfaceFarRegion region, math::Vector2 minimum, math::Vector2 maximum, const SurfacePatch* source) {
        if (minimum.x == maximum.x || minimum.y == maximum.y) return;
        auto patch = MakeFarPatch(inner, minimum, maximum);
        if (source) {
            patch.depth = source->depth;
            patch.x = source->x;
            patch.z = source->z;
            patch.morphCellSize = source->morphCellSize;
        }
        result.push_back({region, std::move(patch)});
    };
    for (uint32_t side = 0; side < 4; ++side) {
        for (const auto& boundary : boundaries[side]) {
            const auto& source = innerLeaves[boundary.leaf];
            switch (side) {
            case 0: append(SurfaceFarRegion::MIN_X, {outerUvMin.x, source.uvMin.y}, {0.0f, source.uvMax.y}, &source); break;
            case 1: append(SurfaceFarRegion::MAX_X, {1.0f, source.uvMin.y}, {outerUvMax.x, source.uvMax.y}, &source); break;
            case 2: append(SurfaceFarRegion::MIN_Z, {source.uvMin.x, outerUvMin.y}, {source.uvMax.x, 0.0f}, &source); break;
            default: append(SurfaceFarRegion::MAX_Z, {source.uvMin.x, 1.0f}, {source.uvMax.x, outerUvMax.y}, &source); break;
            }
        }
    }
    append(SurfaceFarRegion::MIN_X_MIN_Z, outerUvMin, {0.0f, 0.0f}, nullptr);
    append(SurfaceFarRegion::MAX_X_MIN_Z, {1.0f, outerUvMin.y}, {outerUvMax.x, 0.0f}, nullptr);
    append(SurfaceFarRegion::MIN_X_MAX_Z, {outerUvMin.x, 1.0f}, {0.0f, outerUvMax.y}, nullptr);
    append(SurfaceFarRegion::MAX_X_MAX_Z, {1.0f, 1.0f}, outerUvMax, nullptr);
    math::Vector2 sharedCell{};
    for (const auto& far : result) {
        if (!WorldBoundsFinite(inner, far.geometry)) return false;
        const auto span = far.geometry.localMax - far.geometry.localMin;
        const math::Vector2 cell{span.x / cellsPerPatch * axisScale.x, span.y / cellsPerPatch * axisScale.y};
        if (!Finite(cell)) return false;
        sharedCell = Maximum(sharedCell, cell);
    }
    for (auto& far : result) {
        auto& patch = far.geometry;
        patch.cellSize = sharedCell;
        patch.edgeCellSize.fill(sharedCell);
        patch.cornerCellSize.fill(sharedCell);
    }
    size_t stripIndex = 0;
    for (uint32_t side = 0; side < 4; ++side) {
        const bool extended = side == 0 ? outerUvMin.x < 0.0f : side == 1 ? outerUvMax.x > 1.0f
            : side == 2 ? outerUvMin.y < 0.0f : outerUvMax.y > 1.0f;
        if (!extended) continue;
        for (const auto& boundary : boundaries[side]) {
            const auto& source = innerLeaves[boundary.leaf];
            auto& far = result[stripIndex++].geometry;
            far.edgeCellSize[side ^ 1u] = source.edgeCellSize[side];
            switch (side) {
            case 0: far.cornerCellSize[1] = source.cornerCellSize[0]; far.cornerCellSize[3] = source.cornerCellSize[2]; break;
            case 1: far.cornerCellSize[0] = source.cornerCellSize[1]; far.cornerCellSize[2] = source.cornerCellSize[3]; break;
            case 2: far.cornerCellSize[2] = source.cornerCellSize[0]; far.cornerCellSize[3] = source.cornerCellSize[1]; break;
            default: far.cornerCellSize[0] = source.cornerCellSize[2]; far.cornerCellSize[1] = source.cornerCellSize[3]; break;
            }
        }
    }
    const std::array<math::Vector2, 4> innerCorners = {
        innerLeaves[boundaries[0].front().leaf].cornerCellSize[0],
        innerLeaves[boundaries[1].front().leaf].cornerCellSize[1],
        innerLeaves[boundaries[0].back().leaf].cornerCellSize[2],
        innerLeaves[boundaries[1].back().leaf].cornerCellSize[3]
    };
    const std::array<std::array<const SurfacePatch*, 2>, 4> cornerLeaves = {{
        {&innerLeaves[boundaries[0].front().leaf], &innerLeaves[boundaries[2].front().leaf]},
        {&innerLeaves[boundaries[1].front().leaf], &innerLeaves[boundaries[2].back().leaf]},
        {&innerLeaves[boundaries[0].back().leaf], &innerLeaves[boundaries[3].front().leaf]},
        {&innerLeaves[boundaries[1].back().leaf], &innerLeaves[boundaries[3].back().leaf]}
    }};
    for (size_t index = stripIndex; index < result.size(); ++index) {
        const uint32_t corner = static_cast<uint32_t>(result[index].region) - 4u;
        auto& geometry = result[index].geometry;
        geometry.cornerCellSize[corner ^ 3u] = innerCorners[corner];
        const auto* first = cornerLeaves[corner][0];
        const auto* second = cornerLeaves[corner][1];
        const auto* coarsest = first->depth <= second->depth ? first : second;
        geometry.depth = coarsest->depth;
        geometry.x = coarsest->x;
        geometry.z = coarsest->z;
        geometry.morphCellSize = coarsest->morphCellSize;
    }
    if (morphEnabled) {
        /// @note 近景と同じ階層移動上限で far strip のカリング箱を広げる。
        /// @see Docs/design/surface-patch-lod.md Hierarchical parent-map movement bound.
        const math::Vector2 morphPadding{2.0f * extent.x / static_cast<float>(cellsPerPatch),
            2.0f * extent.y / static_cast<float>(cellsPerPatch)};
        for (auto& far : result) {
            far.geometry.aabbMin.x -= morphPadding.x;
            far.geometry.aabbMin.z -= morphPadding.y;
            far.geometry.aabbMax.x += morphPadding.x;
            far.geometry.aabbMax.z += morphPadding.y;
        }
    }
    for (const auto& far : result)
        if (!Finite(far.geometry.morphCellSize) || !WorldBoundsFinite(inner, far.geometry)) return false;
    out = std::move(result);
    return true;
}

} /// @note namespace fbzz::renderer
