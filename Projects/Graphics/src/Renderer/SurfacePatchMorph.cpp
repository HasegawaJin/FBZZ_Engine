/// @file    SurfacePatchMorph.cpp
/// @brief   center-fan 親写像、画面密度係数、階層位置の CPU 実装。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <Graphics/Renderer/SurfacePatchMorph.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace fbzz::renderer {
namespace {

constexpr float MAX_GRID_COORDINATE = 10000000.0f;

bool Finite(math::Vector2 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool Finite(math::Vector3 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

float Dot(math::Vector3 a, math::Vector3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float Cross(math::Vector2 a, math::Vector2 b)
{
    return a.x * b.y - a.y * b.x;
}

float SmoothStep(float minimum, float maximum, float value)
{
    if (value <= minimum) return 0.0f;
    if (value >= maximum) return 1.0f;
    const float t = (value - minimum) / (maximum - minimum);
    return t * t * (3.0f - 2.0f * t);
}

bool ValidAxisMask(uint8_t axisMask)
{
    return (axisMask & ~SURFACE_MORPH_AXIS_BOTH) == 0;
}

int64_t FloorHalf(int64_t value)
{
    return value >= 0 ? value / 2 : -((-value + 1) / 2);
}

bool IsOdd(int64_t value)
{
    return value % 2 != 0;
}

int64_t PositiveParity(int64_t value)
{
    return ((value % 2) + 2) % 2;
}

/// @brief Map an integer fan vertex to the parent lattice with the parity-safe center target.
/// @note The odd/odd center chooses one horizontal endpoint so four child index sets reproduce the parent fan exactly.
/// @see Docs/design/surface-patch-lod.md Center-fan parent map.
math::Vector2 CollapseVertex(int64_t x, int64_t z, uint8_t axisMask)
{
    const int64_t halfX = FloorHalf(x);
    const int64_t halfZ = FloorHalf(z);
    if (axisMask == SURFACE_MORPH_AXIS_BOTH) {
        if (IsOdd(x) && IsOdd(z)) {
            if (PositiveParity(halfX + halfZ) == 0)
                return {static_cast<float>(2 * halfX), static_cast<float>(2 * halfZ)};
            return {static_cast<float>(2 * halfX + 2), static_cast<float>(2 * halfZ)};
        }
        return {static_cast<float>(IsOdd(x) ? 2 * halfX : x),
                static_cast<float>(IsOdd(z) ? 2 * halfZ : z)};
    }
    if (axisMask == SURFACE_MORPH_AXIS_X) {
        if (!IsOdd(x)) return {static_cast<float>(x), static_cast<float>(z)};
        const int64_t targetX = IsOdd(z)
            ? (PositiveParity(halfX) == 0 ? 2 * halfX + 2 : 2 * halfX)
            : 2 * halfX;
        return {static_cast<float>(targetX), static_cast<float>(z)};
    }
    if (axisMask == SURFACE_MORPH_AXIS_Z) {
        if (!IsOdd(z)) return {static_cast<float>(x), static_cast<float>(z)};
        const int64_t targetZ = IsOdd(x)
            ? (PositiveParity(halfZ) == 0 ? 2 * halfZ + 2 : 2 * halfZ)
            : 2 * halfZ;
        return {static_cast<float>(x), static_cast<float>(targetZ)};
    }
    return {static_cast<float>(x), static_cast<float>(z)};
}

struct FanTriangle {
    std::array<math::Vector2, 3> source{};
    std::array<math::Vector2, 3> target{};
    std::array<float, 3> weight{};
};

/// @brief Select the original center-fan triangle and its barycentric coordinates.
/// @see Docs/design/surface-patch-lod.md Center-fan parent map.
bool EvaluateFanTriangle(math::Vector2 gridCoordinate, uint8_t axisMask, FanTriangle& out)
{
    const int64_t blockX = static_cast<int64_t>(std::floor(static_cast<double>(gridCoordinate.x) * 0.5)) * 2;
    const int64_t blockZ = static_cast<int64_t>(std::floor(static_cast<double>(gridCoordinate.y) * 0.5)) * 2;
    const math::Vector2 block{static_cast<float>(blockX), static_cast<float>(blockZ)};
    const math::Vector2 local{gridCoordinate.x - block.x, gridCoordinate.y - block.y};
    const math::Vector2 relative{local.x - 1.0f, local.y - 1.0f};
    constexpr std::array<std::array<int64_t, 2>, 8> PERIMETER = {{
        {0, 0}, {0, 1}, {0, 2}, {1, 2}, {2, 2}, {2, 1}, {2, 0}, {1, 0}
    }};
    const bool dominantX = std::abs(relative.x) >= std::abs(relative.y);
    size_t sector = 0;
    if (relative.x < 0.0f)
        sector = relative.y < 0.0f ? (dominantX ? 0u : 7u) : (dominantX ? 1u : 2u);
    else
        sector = relative.y > 0.0f ? (dominantX ? 4u : 3u) : (dominantX ? 5u : 6u);
    const auto& pointA = PERIMETER[sector];
    const auto& pointB = PERIMETER[(sector + 1u) % PERIMETER.size()];
    const math::Vector2 a{static_cast<float>(pointA[0]) - 1.0f,
                          static_cast<float>(pointA[1]) - 1.0f};
    const math::Vector2 b{static_cast<float>(pointB[0]) - 1.0f,
                          static_cast<float>(pointB[1]) - 1.0f};
    const float denominator = Cross(a, b);
    if (std::abs(denominator) <= 1.0e-8f) return false;
    const float weightA = Cross(relative, b) / denominator;
    const float weightB = Cross(a, relative) / denominator;
    const float weightCenter = 1.0f - weightA - weightB;
    const math::Vector2 center = block + math::Vector2{1.0f, 1.0f};
    const math::Vector2 sourceA = block + math::Vector2{static_cast<float>(pointA[0]), static_cast<float>(pointA[1])};
    const math::Vector2 sourceB = block + math::Vector2{static_cast<float>(pointB[0]), static_cast<float>(pointB[1])};
    out.source = {center, sourceA, sourceB};
    out.target = {
        CollapseVertex(blockX + 1, blockZ + 1, axisMask),
        CollapseVertex(blockX + pointA[0], blockZ + pointA[1], axisMask),
        CollapseVertex(blockX + pointB[0], blockZ + pointB[1], axisMask)
    };
    out.weight = {weightCenter, weightA, weightB};
    return std::isfinite(weightCenter) && std::isfinite(weightA) && std::isfinite(weightB);
}

math::Vector2 AddScaled(math::Vector2 value, math::Vector2 axis, float scale)
{
    return {value.x + axis.x * scale, value.y + axis.y * scale};
}

math::Vector3 AddScaled(math::Vector3 value, math::Vector3 axis, float scale)
{
    return {value.x + axis.x * scale, value.y + axis.y * scale, value.z + axis.z * scale};
}

math::Vector2 RootUvAtOffset(math::Vector2 rootUv, math::Vector2 uvPerGridX,
    math::Vector2 uvPerGridZ, math::Vector2 gridOffset)
{
    return AddScaled(AddScaled(rootUv, uvPerGridX, gridOffset.x), uvPerGridZ, gridOffset.y);
}

bool ValidSettings(const SurfacePatchLodSettings& settings)
{
    return std::isfinite(settings.targetCellPixels) && settings.targetCellPixels > 0.0f
        && std::isfinite(settings.nearDistance) && settings.nearDistance >= 0.0f
        && std::isfinite(settings.nearCellSize) && settings.nearCellSize > 0.0f
        && std::isfinite(settings.morphTransitionWidth)
        && settings.morphTransitionWidth >= 0.01f && settings.morphTransitionWidth <= 1.0f;
}

bool ValidView(const SurfacePatchView& view, math::Vector3& normalizedForward)
{
    if (!Finite(view.cameraPosition) || !Finite(view.cameraForward)
        || !std::isfinite(view.cameraForward.LengthSq()) || view.cameraForward.LengthSq() <= 1.0e-8f
        || !std::isfinite(view.projectionScalePixels) || view.projectionScalePixels <= 0.0f
        || !std::isfinite(view.minProjectionDistance) || view.minProjectionDistance <= 0.0f)
        return false;
    normalizedForward = view.cameraForward.Normalized();
    return Finite(normalizedForward);
}

}

/// @brief Extend the parent vertex map linearly across the selected fan triangle.
/// @see Docs/design/surface-patch-lod.md Center-fan parent map.
bool EvaluateSurfacePatchParentMap(math::Vector2 gridCoordinate, uint8_t axisMask,
                                   math::Vector2& out)
{
    if (!Finite(gridCoordinate) || std::abs(gridCoordinate.x) > MAX_GRID_COORDINATE
        || std::abs(gridCoordinate.y) > MAX_GRID_COORDINATE || !ValidAxisMask(axisMask))
        return false;
    if (axisMask == 0) { out = gridCoordinate; return true; }
    FanTriangle triangle;
    if (!EvaluateFanTriangle(gridCoordinate, axisMask, triangle)) return false;
    out = {};
    for (size_t index = 0; index < triangle.target.size(); ++index) {
        out.x += triangle.target[index].x * triangle.weight[index];
        out.y += triangle.target[index].y * triangle.weight[index];
    }
    return Finite(out);
}

bool ComputeSurfacePatchMorphAlpha(math::Vector3 worldPosition, math::Vector2 parentCellSize,
    const SurfacePatchLodSettings& settings, const SurfacePatchView& view,
    float& outAlpha)
{
    math::Vector3 forward;
    if (!Finite(worldPosition) || !Finite(parentCellSize)
        || !ValidSettings(settings) || !ValidView(view, forward)
        || parentCellSize.x <= 0.0f || parentCellSize.y <= 0.0f)
        return false;
    if (!settings.morphEnabled) { outAlpha = 0.0f; return true; }

    const float cell = (std::max)(parentCellSize.x, parentCellSize.y);
    const math::Vector3 delta = worldPosition - view.cameraPosition;
    const float depth = Dot(delta, forward);
    const float projectedDepth = view.orthographic
        ? 1.0f : (std::max)(depth, view.minProjectionDistance);
    const double projected = static_cast<double>(cell) * view.projectionScalePixels
        / (static_cast<double>(projectedDepth) * settings.targetCellPixels);
    const float distance = delta.Length();
    float nearWeight = 0.0f;
    if (settings.nearDistance > 0.0f)
        nearWeight = 1.0f - SmoothStep(settings.nearDistance, settings.nearDistance * 2.0f, distance);
    const double nearMetric = static_cast<double>(cell) / settings.nearCellSize * nearWeight;
    const double score = (std::max)(projected, nearMetric);
    if (!std::isfinite(score)) return false;
    const double alpha = (1.0 + static_cast<double>(settings.morphTransitionWidth) - score)
        / settings.morphTransitionWidth;
    outAlpha = static_cast<float>((std::clamp)(alpha, 0.0, 1.0));
    return true;
}

bool MorphSurfacePatchGridCoordinate(math::Vector2 gridCoordinate, std::span<const float> alpha,
    uint8_t axisMask, math::Vector2& out)
{
    if (!Finite(gridCoordinate) || std::abs(gridCoordinate.x) > MAX_GRID_COORDINATE
        || std::abs(gridCoordinate.y) > MAX_GRID_COORDINATE || !ValidAxisMask(axisMask)
        || alpha.size() > 10)
        return false;
    for (float value : alpha)
        if (!std::isfinite(value) || value < 0.0f || value > 1.0f) return false;
    math::Vector2 result = gridCoordinate;
    for (size_t level = 0; level < alpha.size(); ++level) {
        const float scale = std::ldexp(1.0f, static_cast<int>(level));
        const math::Vector2 coarseInput{result.x / scale, result.y / scale};
        math::Vector2 parent;
        if (!EvaluateSurfacePatchParentMap(coarseInput, axisMask, parent)) return false;
        const math::Vector2 target = parent * scale;
        const float weight = alpha[level];
        result = {result.x + (target.x - result.x) * weight,
                  result.y + (target.y - result.y) * weight};
        if (!Finite(result)) return false;
    }
    out = result;
    return true;
}

bool MorphSurfacePatchGridCoordinateDensity(math::Vector2 gridCoordinate,
    math::Vector2 rootUv, math::Vector2 rootUvPerGridX, math::Vector2 rootUvPerGridZ,
    math::Vector3 originalWorldPosition, math::Vector3 worldRootUAxis,
    math::Vector3 worldRootVAxis, math::Vector2 nativeCellSize, math::Vector2 sharedBand,
    uint32_t levels, const SurfacePatchLodSettings& settings, const SurfacePatchView& view,
    uint8_t axisMask, math::Vector2& outGridCoordinate, math::Vector2& outRootUv,
    math::Vector2& outCellSize)
{
    if (!Finite(gridCoordinate) || std::abs(gridCoordinate.x) > MAX_GRID_COORDINATE
        || std::abs(gridCoordinate.y) > MAX_GRID_COORDINATE || !Finite(rootUv)
        || !Finite(rootUvPerGridX) || !Finite(rootUvPerGridZ) || !Finite(originalWorldPosition)
        || !Finite(worldRootUAxis) || !Finite(worldRootVAxis) || !Finite(nativeCellSize)
        || !Finite(sharedBand) || nativeCellSize.x <= 0.0f || nativeCellSize.y <= 0.0f
        || sharedBand.x < 0.0f || sharedBand.y < 0.0f || levels > settings.maxDepth
        || levels > 10u || !ValidSettings(settings) || !ValidAxisMask(axisMask))
        return false;
    math::Vector3 normalizedForward;
    if (!ValidView(view, normalizedForward) || !Finite(normalizedForward)) return false;
    if (!settings.morphEnabled) {
        outGridCoordinate = gridCoordinate;
        outRootUv = rootUv;
        outCellSize = sharedBand;
        return true;
    }

    math::Vector2 current = gridCoordinate;
    for (uint32_t level = 0; level < levels && axisMask != 0; ++level) {
        const float scale = std::ldexp(1.0f, static_cast<int>(level));
        const math::Vector2 levelScale{
            (axisMask & SURFACE_MORPH_AXIS_X) != 0 ? scale : 1.0f,
            (axisMask & SURFACE_MORPH_AXIS_Z) != 0 ? scale : 1.0f
        };
        const math::Vector2 levelCoordinate{current.x / levelScale.x, current.y / levelScale.y};
        FanTriangle triangle;
        if (!EvaluateFanTriangle(levelCoordinate, axisMask, triangle)) return false;
        const math::Vector2 currentUv = RootUvAtOffset(rootUv, rootUvPerGridX, rootUvPerGridZ,
            {current.x - gridCoordinate.x, current.y - gridCoordinate.y});
        const math::Vector2 parentCellSize = nativeCellSize * std::ldexp(1.0f, static_cast<int>(level + 1u));
        if (!Finite(parentCellSize)) return false;
        math::Vector2 mapped{};
        for (size_t vertex = 0; vertex < triangle.source.size(); ++vertex) {
            const math::Vector2 source{
                triangle.source[vertex].x * levelScale.x,
                triangle.source[vertex].y * levelScale.y
            };
            const math::Vector2 target{
                triangle.target[vertex].x * levelScale.x,
                triangle.target[vertex].y * levelScale.y
            };
            const math::Vector2 vertexUv = RootUvAtOffset(currentUv, rootUvPerGridX,
                rootUvPerGridZ, {source.x - current.x, source.y - current.y});
            const math::Vector3 world = AddScaled(
                AddScaled(originalWorldPosition, worldRootUAxis, vertexUv.x - rootUv.x),
                worldRootVAxis, vertexUv.y - rootUv.y);
            float alpha = 0.0f;
            if (!ComputeSurfacePatchMorphAlpha(world, parentCellSize, settings, view, alpha)) return false;
            mapped.x += (source.x + (target.x - source.x) * alpha) * triangle.weight[vertex];
            mapped.y += (source.y + (target.y - source.y) * alpha) * triangle.weight[vertex];
        }
        if (!Finite(mapped)) return false;
        current = mapped;
    }

    const math::Vector2 finalUv = RootUvAtOffset(rootUv, rootUvPerGridX, rootUvPerGridZ,
        {current.x - gridCoordinate.x, current.y - gridCoordinate.y});
    const math::Vector3 finalWorld = AddScaled(
        AddScaled(originalWorldPosition, worldRootUAxis, finalUv.x - rootUv.x),
        worldRootVAxis, finalUv.y - rootUv.y);
    if (!Finite(finalUv) || !Finite(finalWorld)) return false;
    math::Vector2 cellSize{
        (std::max)(nativeCellSize.x, sharedBand.x),
        (std::max)(nativeCellSize.y, sharedBand.y)
    };
    for (uint32_t level = 0; level < levels; ++level) {
        const float scale = std::ldexp(1.0f, static_cast<int>(level + 1u));
        const math::Vector2 parentCellSize = nativeCellSize * scale;
        float alpha = 0.0f;
        if (!Finite(parentCellSize)
            || !ComputeSurfacePatchMorphAlpha(finalWorld, parentCellSize, settings, view, alpha))
            return false;
        if (parentCellSize.x > cellSize.x)
            cellSize.x += (parentCellSize.x - cellSize.x) * alpha;
        if (parentCellSize.y > cellSize.y)
            cellSize.y += (parentCellSize.y - cellSize.y) * alpha;
    }
    if (!Finite(cellSize)) return false;
    outGridCoordinate = current;
    outRootUv = finalUv;
    outCellSize = cellSize;
    return true;
}

bool ResolveSurfacePatchMorphCellSize(math::Vector2 nativeCellSize, math::Vector2 sharedBand,
    std::span<const float> alpha, math::Vector2& out)
{
    if (!Finite(nativeCellSize) || !Finite(sharedBand)
        || alpha.size() > 10 || nativeCellSize.x < 0.0f || nativeCellSize.y < 0.0f
        || sharedBand.x < 0.0f || sharedBand.y < 0.0f)
        return false;
    math::Vector2 morphed = {
        (std::max)(nativeCellSize.x, sharedBand.x),
        (std::max)(nativeCellSize.y, sharedBand.y)
    };
    for (size_t level = 0; level < alpha.size(); ++level) {
        const float value = alpha[level];
        if (!std::isfinite(value) || value < 0.0f || value > 1.0f) return false;
        const float scale = std::ldexp(1.0f, static_cast<int>(level + 1u));
        const math::Vector2 parentCellSize = nativeCellSize * scale;
        if (!Finite(parentCellSize)) return false;
        if (parentCellSize.x > morphed.x)
            morphed.x += (parentCellSize.x - morphed.x) * value;
        if (parentCellSize.y > morphed.y)
            morphed.y += (parentCellSize.y - morphed.y) * value;
        if (!Finite(morphed)) return false;
    }
    out = morphed;
    return true;
}

}
