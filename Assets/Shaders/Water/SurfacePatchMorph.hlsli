/// @file    SurfacePatchMorph.hlsli
/// @brief   Shared center-fan parent maps and continuous surface LOD morph sampling.
/// @author  Hasegawa Jin
/// @date    2026-10-07
#ifndef FBZZ_SURFACE_PATCH_MORPH_HLSLI
#define FBZZ_SURFACE_PATCH_MORPH_HLSLI

static const uint SURFACE_MORPH_AXIS_X = 1u;
static const uint SURFACE_MORPH_AXIS_Z = 2u;
static const uint SURFACE_MORPH_AXIS_BOTH = 3u;
static const float2 SURFACE_PATCH_MORPH_PERIMETER[8] = {
    float2(0.0f, 0.0f), float2(0.0f, 1.0f), float2(0.0f, 2.0f), float2(1.0f, 2.0f),
    float2(2.0f, 2.0f), float2(2.0f, 1.0f), float2(2.0f, 0.0f), float2(1.0f, 0.0f)
};

struct SurfacePatchMorphSample
{
    float2 uv;
    float2 cellSize;
};

/// @brief Collapse one integer lattice vertex onto its center-fan parent lattice.
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry GPU Gems 2, 2.3.2 shared grid footprints.
float2 SurfacePatchMorphCollapseVertex(float2 gridCoordinate, uint axisMask)
{
    float2 halfPoint = floor(gridCoordinate * 0.5f);
    bool oddX = fmod(abs(gridCoordinate.x), 2.0f) > 0.5f;
    bool oddZ = fmod(abs(gridCoordinate.y), 2.0f) > 0.5f;
    if (axisMask == SURFACE_MORPH_AXIS_BOTH)
    {
        if (oddX && oddZ)
        {
            bool lower = fmod(abs(halfPoint.x + halfPoint.y), 2.0f) < 0.5f;
            return lower ? 2.0f * halfPoint : 2.0f * halfPoint + float2(2.0f, 0.0f);
        }
        return float2(oddX ? 2.0f * halfPoint.x : gridCoordinate.x,
                      oddZ ? 2.0f * halfPoint.y : gridCoordinate.y);
    }
    if (axisMask == SURFACE_MORPH_AXIS_X)
    {
        if (!oddX) return gridCoordinate;
        float targetX = oddZ && fmod(abs(halfPoint.x), 2.0f) < 0.5f
            ? 2.0f * halfPoint.x + 2.0f : 2.0f * halfPoint.x;
        return float2(targetX, gridCoordinate.y);
    }
    if (axisMask == SURFACE_MORPH_AXIS_Z)
    {
        if (!oddZ) return gridCoordinate;
        float targetZ = oddX && fmod(abs(halfPoint.y), 2.0f) < 0.5f
            ? 2.0f * halfPoint.y + 2.0f : 2.0f * halfPoint.y;
        return float2(gridCoordinate.x, targetZ);
    }
    return gridCoordinate;
}

/// @brief Piecewise-linear extension of the lattice map over its eight center-fan triangles.
float2 SurfacePatchMorphParentMap(float2 gridCoordinate, uint axisMask)
{
    if (axisMask == 0u) return gridCoordinate;
    float2 block = 2.0f * floor(gridCoordinate * 0.5f);
    float2 local = gridCoordinate - block;
    float2 relative = local - 1.0f;
    float2 centerTarget = SurfacePatchMorphCollapseVertex(block + 1.0f, axisMask) - block;
    bool dominantX = abs(relative.x) >= abs(relative.y);
    uint sector;
    if (relative.x < 0.0f)
        sector = relative.y < 0.0f ? (dominantX ? 0u : 7u) : (dominantX ? 1u : 2u);
    else
        sector = relative.y > 0.0f ? (dominantX ? 4u : 3u) : (dominantX ? 5u : 6u);
    float2 a = SURFACE_PATCH_MORPH_PERIMETER[sector] - 1.0f;
    float2 b = SURFACE_PATCH_MORPH_PERIMETER[(sector + 1u) & 7u] - 1.0f;
    float denominator = a.x * b.y - a.y * b.x;
    float weightA = (relative.x * b.y - relative.y * b.x) / denominator;
    float weightB = (a.x * relative.y - a.y * relative.x) / denominator;
    float weightCenter = 1.0f - weightA - weightB;
    float2 targetA = SurfacePatchMorphCollapseVertex(
        block + SURFACE_PATCH_MORPH_PERIMETER[sector], axisMask) - block;
    float2 targetB = SurfacePatchMorphCollapseVertex(
        block + SURFACE_PATCH_MORPH_PERIMETER[(sector + 1u) & 7u], axisMask) - block;
    return block + centerTarget * weightCenter + targetA * weightA + targetB * weightB;
}

/// @brief Return the source fan triangle, collapsed targets, and barycentric weights at a lattice point.
/// @see Docs/design/surface-patch-lod.md Center-fan parent map.
void SurfacePatchMorphFanTriangle(float2 gridCoordinate, uint axisMask,
    out float2 source[3], out float2 target[3], out float weights[3])
{
    float2 block = 2.0f * floor(gridCoordinate * 0.5f);
    float2 local = gridCoordinate - block;
    float2 relative = local - 1.0f;
    bool dominantX = abs(relative.x) >= abs(relative.y);
    uint sector;
    if (relative.x < 0.0f)
        sector = relative.y < 0.0f ? (dominantX ? 0u : 7u) : (dominantX ? 1u : 2u);
    else
        sector = relative.y > 0.0f ? (dominantX ? 4u : 3u) : (dominantX ? 5u : 6u);
    float2 a = SURFACE_PATCH_MORPH_PERIMETER[sector] - 1.0f;
    float2 b = SURFACE_PATCH_MORPH_PERIMETER[(sector + 1u) & 7u] - 1.0f;
    float denominator = a.x * b.y - a.y * b.x;
    float weightA = (relative.x * b.y - relative.y * b.x) / denominator;
    float weightB = (a.x * relative.y - a.y * relative.x) / denominator;
    weights[0] = 1.0f - weightA - weightB;
    weights[1] = weightA;
    weights[2] = weightB;
    source[0] = block + 1.0f;
    source[1] = block + SURFACE_PATCH_MORPH_PERIMETER[sector];
    source[2] = block + SURFACE_PATCH_MORPH_PERIMETER[(sector + 1u) & 7u];
    target[0] = SurfacePatchMorphCollapseVertex(source[0], axisMask);
    target[1] = SurfacePatchMorphCollapseVertex(source[1], axisMask);
    target[2] = SurfacePatchMorphCollapseVertex(source[2], axisMask);
}

/// @brief Evaluate one density field sample; alpha is one whenever its parent cell is acceptable.
float SurfacePatchMorphAlpha(float3 worldPosition, float2 parentCellSize,
    float4 morphView, float4 morphForward, float4 morphMetric, float transitionWidth)
{
    float cell = max(parentCellSize.x, parentCellSize.y);
    float3 delta = worldPosition - morphView.xyz;
    float depth = dot(delta, morphForward.xyz);
    float projectedDepth = morphForward.w > 0.5f
        ? 1.0f : max(depth, morphMetric.x);
    float projected = cell * morphView.w / (projectedDepth * morphMetric.y);
    float distance = length(delta);
    float nearWeight = morphMetric.z > 0.0f
        ? 1.0f - smoothstep(morphMetric.z, morphMetric.z * 2.0f, distance) : 0.0f;
    float nearMetric = cell / morphMetric.w * nearWeight;
    float score = max(projected, nearMetric);
    return saturate((1.0f + transitionWidth - score) / transitionWidth);
}

/// @brief Compose per-vertex center-fan morphing and return its continuous world wave band.
/// @note Density is evaluated at each source fan vertex, then the three blended targets are extended linearly.
/// @note Far strips scale only their tangent grid axis; their outward coordinate stays in the local 0..N row.
/// @note The effective band samples the density field at finalRootUv so collapsed child and parent endpoints agree.
SurfacePatchMorphSample SurfacePatchMorphUv(float2 rootUv, float2 unitUv,
    float3 originalWorldPosition, float3 worldRootUAxis, float3 worldRootVAxis,
    float4 patchRect, float cellsPerPatch, float4 morphParams, float4 morphView,
    float4 morphForward, float4 morphMetric, float2 nativeCellSize, float2 sharedBand)
{
    SurfacePatchMorphSample result;
    result.uv = rootUv;
    result.cellSize = sharedBand;
    if (morphParams.x < 0.5f) return result;

    uint levels = (uint)round(morphParams.z);
    uint axisMask = (uint)round(morphParams.w);
    float gridScale = cellsPerPatch * exp2((float)levels);
    float2 gridCoordinate = rootUv * gridScale;
    float2 rootUvPerGridX = float2(1.0f / gridScale, 0.0f);
    float2 rootUvPerGridZ = float2(0.0f, 1.0f / gridScale);
    if (axisMask == SURFACE_MORPH_AXIS_Z)
    {
        gridCoordinate = float2(unitUv.x * cellsPerPatch, rootUv.y * gridScale);
        rootUvPerGridX = float2((patchRect.z - patchRect.x) / cellsPerPatch, 0.0f);
    }
    else if (axisMask == SURFACE_MORPH_AXIS_X)
    {
        gridCoordinate = float2(rootUv.x * gridScale, unitUv.y * cellsPerPatch);
        rootUvPerGridZ = float2(0.0f, (patchRect.w - patchRect.y) / cellsPerPatch);
    }

    float2 current = gridCoordinate;
    [loop]
    for (uint level = 0u; level < levels; ++level)
    {
        if (axisMask == 0u) break;
        float levelScale = exp2((float)level);
        float2 axisScale = float2((axisMask & SURFACE_MORPH_AXIS_X) != 0u ? levelScale : 1.0f,
                                  (axisMask & SURFACE_MORPH_AXIS_Z) != 0u ? levelScale : 1.0f);
        float2 levelCoordinate = current / axisScale;
        float2 source[3], target[3];
        float weights[3];
        SurfacePatchMorphFanTriangle(levelCoordinate, axisMask, source, target, weights);
        float2 currentUv = rootUv + rootUvPerGridX * (current.x - gridCoordinate.x)
            + rootUvPerGridZ * (current.y - gridCoordinate.y);
        float2 parentCellSize = nativeCellSize * exp2((float)(level + 1u));
        float2 mapped = 0.0f;
        [unroll]
        for (uint vertex = 0u; vertex < 3u; ++vertex)
        {
            float2 sourceGrid = source[vertex] * axisScale;
            float2 targetGrid = target[vertex] * axisScale;
            float2 vertexUv = currentUv + rootUvPerGridX * (sourceGrid.x - current.x)
                + rootUvPerGridZ * (sourceGrid.y - current.y);
            float3 vertexWorld = originalWorldPosition
                + worldRootUAxis * (vertexUv.x - rootUv.x)
                + worldRootVAxis * (vertexUv.y - rootUv.y);
            float alpha = SurfacePatchMorphAlpha(vertexWorld, parentCellSize,
                morphView, morphForward, morphMetric, morphParams.y);
            mapped += lerp(sourceGrid, targetGrid, alpha) * weights[vertex];
        }
        current = mapped;
    }

    float2 finalRootUv = rootUv + rootUvPerGridX * (current.x - gridCoordinate.x)
        + rootUvPerGridZ * (current.y - gridCoordinate.y);
    float3 finalWorld = originalWorldPosition
        + worldRootUAxis * (finalRootUv.x - rootUv.x)
        + worldRootVAxis * (finalRootUv.y - rootUv.y);
    float2 morphedCellSize = max(nativeCellSize, sharedBand);
    [loop]
    for (uint bandLevel = 0u; bandLevel < levels; ++bandLevel)
    {
        float2 parentCellSize = nativeCellSize * exp2((float)(bandLevel + 1u));
        float alpha = SurfacePatchMorphAlpha(finalWorld, parentCellSize,
            morphView, morphForward, morphMetric, morphParams.y);
        if (parentCellSize.x > morphedCellSize.x)
            morphedCellSize.x = lerp(morphedCellSize.x, parentCellSize.x, alpha);
        if (parentCellSize.y > morphedCellSize.y)
            morphedCellSize.y = lerp(morphedCellSize.y, parentCellSize.y, alpha);
    }
    result.uv = finalRootUv;
    result.cellSize = morphedCellSize;
    return result;
}

#endif
