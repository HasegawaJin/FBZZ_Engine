/// @file    WaterMedium.hlsli
/// @brief   View-local water atlas/table lookup and RGB Beer transmittance helpers.
/// @author  Hasegawa Jin
/// @date    2026-10-09
#ifndef FBZZ_RENDERING_WATER_MEDIUM_HLSLI
#define FBZZ_RENDERING_WATER_MEDIUM_HLSLI

#include "Common/BindlessIndices.hlsli"

/// @note PS t14/t15 are view-owned water medium data. VS particle t14/t15 and clustered t29/t30 are separate stages.
#if BINDLESS_SUPPORTED
FBZZ_SBUFFER_T(float4, g_waterMediumTable, 14);
FBZZ_TEX2D_T(float4, g_waterMediumAtlas, 15);
#else
StructuredBuffer<float4> g_waterMediumTable : register(t14);
Texture2D<float4> g_waterMediumAtlas : register(t15);
#endif

static const uint FBZZ_WATER_MEDIUM_TABLE_MAGIC = 0x574D;
static const uint FBZZ_WATER_MEDIUM_HEADER_ROWS = 5;
static const uint FBZZ_WATER_MEDIUM_BODY_ROWS = 12;
static const uint FBZZ_WATER_MEDIUM_GRID_RESOLUTION = 16;

/// @brief Validated dimensions shared by point queries in one shader operation.
struct FBZZWaterMediumQueryContext
{
    uint bodyCount;
    uint tableRows;
    uint atlasWidth;
    uint atlasHeight;
};

/// @brief A resolved water-medium sample at a world-space receiver.
struct FBZZWaterMediumSample
{
    uint ownerIndex;
    float surfaceY;
    float depth;
    float jacobian;
    float3 absorption;
    float3 scattering;
    float phaseG;
    float lightStrength;
    float transitionDepth;
    float maxDistance;
    float causticsIntensity;
    float distortion;
    float3 plumeScattering;
};

/// @brief Check the pass-local table and atlas descriptors before querying either resource.
bool FBZZ_WaterMediumBindingsValid()
{
#if BINDLESS_SUPPORTED
    return IsBindlessValid(FbzzPixelSlot(14)) && IsBindlessValid(FbzzPixelSlot(15));
#else
    return true;
#endif
}

/// @brief Read a validated float4 row from the medium table.
float4 FBZZ_WaterMediumRow(uint row)
{
    return g_waterMediumTable[row];
}

/// @brief Validate the packed table header and return its bounded dimensions.
bool FBZZ_WaterMediumHeader(out FBZZWaterMediumQueryContext context)
{
    context = (FBZZWaterMediumQueryContext)0;
    if (!FBZZ_WaterMediumBindingsValid()) return false;
    uint rowCount = 0u;
    uint strideBytes = 0u;
    g_waterMediumTable.GetDimensions(rowCount, strideBytes);
    if (rowCount < FBZZ_WATER_MEDIUM_HEADER_ROWS || strideBytes != 16u) return false;
    const float4 header = FBZZ_WaterMediumRow(0u);
    if ((uint)round(header.x) != FBZZ_WATER_MEDIUM_TABLE_MAGIC || (uint)round(header.y) != 1u)
        return false;
    const uint bodyCount = (uint)round(header.z);
    const uint2 atlasSize = (uint2)round(FBZZ_WaterMediumRow(4u).xy);
    if (bodyCount > 256u || any(atlasSize == 0u) || any(atlasSize > 16384u)
        || FBZZ_WATER_MEDIUM_HEADER_ROWS + bodyCount * FBZZ_WATER_MEDIUM_BODY_ROWS > rowCount)
        return false;
    context.bodyCount = bodyCount;
    context.tableRows = rowCount;
    context.atlasWidth = atlasSize.x;
    context.atlasHeight = atlasSize.y;
    return true;
}

/// @brief Read conservative Y bounds for optical queries or for queries that also accept caustics-only owners.
bool FBZZ_WaterMediumVerticalBounds(bool allowCausticsOnly, out float2 bounds)
{
    bounds = 0.0f;
    const float mode = FBZZ_WaterMediumRow(2u).w;
    if (!(mode > 0.5f)) return false;
    bounds = FBZZ_WaterMediumRow(4u).zw;
    /// @note Mode 1 tables carry the all-body bounds only; mode 2 also carries a certified optical upper bound.
    if (!allowCausticsOnly && mode > 1.5f)
        bounds.y = FBZZ_WaterMediumRow(3u).x;
    return true;
}

/// @brief Resolve one owner's atlas sample, requiring nearest-pixel ownership before blending valid neighbors.
bool FBZZ_WaterAtlasSampleWithFootprint(uint bodyIndex, float2 worldXZ,
                           FBZZWaterMediumQueryContext context, out float4 sample, out float bottomRange)
{
    sample = 0.0f;
    bottomRange = 0.0f;
    const uint bodyOffset = FBZZ_WATER_MEDIUM_HEADER_ROWS + bodyIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
    if (bodyIndex >= context.bodyCount || bodyOffset + FBZZ_WATER_MEDIUM_BODY_ROWS > context.tableRows)
        return false;
    const float4 worldBounds = FBZZ_WaterMediumRow(bodyOffset + 9u);
    const float2 worldSpan = worldBounds.zw - worldBounds.xy;
    if (any(worldXZ < worldBounds.xy) || any(worldXZ > worldBounds.zw)
        || any(worldSpan <= 1.0e-5f)) return false;
    const float4 tileUv = FBZZ_WaterMediumRow(bodyOffset + 8u);
    const float2 localUv = saturate((worldXZ - worldBounds.xy) / worldSpan);
    const float2 atlasSize = float2(context.atlasWidth, context.atlasHeight);
    const float2 rawPixel = (tileUv.xy + localUv * (tileUv.zw - tileUv.xy)) * atlasSize - 0.5f;
    const int2 pixelMin = (int2)floor(tileUv.xy * atlasSize);
    const int2 pixelMax = max(pixelMin, (int2)ceil(tileUv.zw * atlasSize) - 1);
    const float2 pixel = clamp(rawPixel, (float2)pixelMin, (float2)pixelMax);
    const int2 p0 = clamp((int2)floor(pixel), pixelMin, pixelMax);
    const int2 p1 = clamp(p0 + 1, pixelMin, pixelMax);
    const int2 nearest = clamp((int2)floor(pixel + 0.5f), pixelMin, pixelMax);
    const float ownerTag = (float)(bodyIndex + 1u);
    const float4 nearestValue = g_waterMediumAtlas.Load(int3(nearest, 0));
    if ((uint)round(nearestValue.a) != bodyIndex + 1u) return false;
    const float2 blend = saturate(pixel - floor(pixel));
    const float weights[4] = {
        (1.0f - blend.x) * (1.0f - blend.y),
        blend.x * (1.0f - blend.y),
        (1.0f - blend.x) * blend.y,
        blend.x * blend.y
    };
    const float4 values[4] = {
        g_waterMediumAtlas.Load(int3(p0.x, p0.y, 0)),
        g_waterMediumAtlas.Load(int3(p1.x, p0.y, 0)),
        g_waterMediumAtlas.Load(int3(p0.x, p1.y, 0)),
        g_waterMediumAtlas.Load(int3(p1.x, p1.y, 0))
    };
    float3 weighted = 0.0f;
    float weightSum = 0.0f;
    float minimumBottom = 3.402823466e+38f;
    float maximumBottom = -3.402823466e+38f;
    [unroll]
    for (uint i = 0u; i < 4u; ++i)
    {
        if ((uint)round(values[i].a) != bodyIndex + 1u) continue;
        const float bottom = values[i].x - values[i].y;
        minimumBottom = min(minimumBottom, bottom);
        maximumBottom = max(maximumBottom, bottom);
        if (weights[i] <= 0.0f) continue;
        weighted += values[i].rgb * weights[i];
        weightSum += weights[i];
    }
    if (weightSum <= 1.0e-6f) return false;
    sample = float4(weighted / weightSum, ownerTag);
    bottomRange = max(maximumBottom - minimumBottom, 0.0f);
    return true;
}

bool FBZZ_WaterAtlasSample(uint bodyIndex, float2 worldXZ,
                           FBZZWaterMediumQueryContext context, out float4 sample)
{
    float unusedBottomRange;
    return FBZZ_WaterAtlasSampleWithFootprint(bodyIndex, worldXZ, context, sample, unusedBottomRange);
}

/// @brief Test whether a point lies inside an owner's transformed dry-volume boxes.
bool FBZZ_WaterPointExcluded(uint bodyIndex, float3 worldPos, FBZZWaterMediumQueryContext context)
{
    const uint bodyOffset = FBZZ_WATER_MEDIUM_HEADER_ROWS + bodyIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
    if (bodyIndex >= context.bodyCount || bodyOffset + FBZZ_WATER_MEDIUM_BODY_ROWS > context.tableRows)
        return true;
    const float4 spans = FBZZ_WaterMediumRow(bodyOffset + 10u);
    const uint first = (uint)round(spans.x);
    const uint count = (uint)round(spans.y);
    const uint exclusionStart = (uint)round(FBZZ_WaterMediumRow(3u).y);
    [loop]
    for (uint i = 0u; i < count; ++i)
    {
        const uint row = exclusionStart + (first + i) * 4u;
        if (row + 3u >= context.tableRows) return true;
        const float4 x = FBZZ_WaterMediumRow(row);
        const float4 y = FBZZ_WaterMediumRow(row + 1u);
        const float4 z = FBZZ_WaterMediumRow(row + 2u);
        const float4 extent = FBZZ_WaterMediumRow(row + 3u);
        const float3 local = float3(dot(x, float4(worldPos, 1.0f)),
                                    dot(y, float4(worldPos, 1.0f)),
                                    dot(z, float4(worldPos, 1.0f)));
        if (all(abs(local) <= extent.xyz)) return true;
    }
    return false;
}

/// @brief Resolve one body at a point without scanning the candidate grid.
bool FBZZ_WaterMediumBodySample(uint bodyIndex, float3 worldPos,
                                FBZZWaterMediumQueryContext context, bool testExclusions,
                                bool allowCausticsOnly, bool receiverBoundary,
                                out FBZZWaterMediumSample result)
{
    result = (FBZZWaterMediumSample)0;
    if (bodyIndex >= context.bodyCount) return false;
    const uint bodyOffset = FBZZ_WATER_MEDIUM_HEADER_ROWS + bodyIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
    if (bodyOffset + FBZZ_WATER_MEDIUM_BODY_ROWS > context.tableRows) return false;
    const float4 flags = FBZZ_WaterMediumRow(bodyOffset + 11u);
    if ((flags.y < 0.5f && (!allowCausticsOnly || flags.z < 0.5f))
        || (testExclusions && FBZZ_WaterPointExcluded(bodyIndex, worldPos, context))) return false;
    float4 atlasValue;
    float bottomRange = 0.0f;
    if (!FBZZ_WaterAtlasSampleWithFootprint(bodyIndex, worldPos.xz, context, atlasValue, bottomRange)) return false;
    const float4 limits = FBZZ_WaterMediumRow(bodyOffset + 6u);
    const float surfaceY = limits.x + atlasValue.x;
    const float depthLimit = max(limits.z, 0.0f);
    const float physicalDepth = min(max(atlasValue.y, 0.0f), depthLimit);
    const float waterDepth = surfaceY - worldPos.y;
    float receiverDepth = physicalDepth;
    /// @note Only Terrain receiver lighting uses its owned atlas footprint and packed-depth error; volume/Composite/Spline queries keep exact depth.
    if (receiverBoundary) {
        const float4 geometry = FBZZ_WaterMediumRow(bodyOffset);
        if (geometry.z > 0.5f && flags.w > 0.5f) {
            const float rounding = max(max(abs(limits.x), depthLimit), 1.0f) * 1.0e-6f;
            const float boundaryError = bottomRange + max(-geometry.w, 0.0f) + rounding;
            receiverDepth = min(physicalDepth + boundaryError, depthLimit);
        }
    }
    if (physicalDepth <= 0.0f || waterDepth <= 0.0f || waterDepth > receiverDepth) return false;
    const float4 optical = FBZZ_WaterMediumRow(bodyOffset + 4u);
    const float4 scattering = FBZZ_WaterMediumRow(bodyOffset + 5u);
    const float4 settings = FBZZ_WaterMediumRow(bodyOffset + 7u);
    const float4 spans = FBZZ_WaterMediumRow(bodyOffset + 10u);
    const uint plumeStart = (uint)round(FBZZ_WaterMediumRow(3u).z);
    const uint plumeOffset = (uint)round(spans.z);
    const uint plumeCount = (uint)round(spans.w);
    float3 plumeScattering = 0.0f;
    [loop]
    for (uint i = 0u; i < plumeCount; ++i)
    {
        const uint row = plumeStart + (plumeOffset + i) * 2u;
        if (row + 1u >= context.tableRows) break;
        const float4 sphere = FBZZ_WaterMediumRow(row);
        const float4 density = FBZZ_WaterMediumRow(row + 1u);
        const float radius = max(sphere.w, 1.0e-4f);
        const float falloff = saturate(1.0f - length(worldPos - sphere.xyz) / radius);
        plumeScattering += max(density.rgb, 0.0f) * max(density.w, 0.0f) * falloff * falloff;
    }
    result.ownerIndex = bodyIndex;
    result.surfaceY = surfaceY;
    result.depth = physicalDepth;
    result.jacobian = atlasValue.z;
    result.absorption = max(optical.rgb, 0.0f);
    result.scattering = max(scattering.rgb, 0.0f);
    result.phaseG = clamp(optical.w, -0.95f, 0.95f);
    result.lightStrength = max(scattering.w, 0.0f);
    result.transitionDepth = max(settings.x, 1.0e-4f);
    result.causticsIntensity = max(settings.y, 0.0f);
    result.distortion = max(settings.z, 0.0f);
    result.maxDistance = max(limits.w, 0.0f);
    result.plumeScattering = plumeScattering;
    return true;
}

bool FBZZ_WaterMediumBodySample(uint bodyIndex, float3 worldPos,
                                FBZZWaterMediumQueryContext context, bool testExclusions,
                                bool allowCausticsOnly, out FBZZWaterMediumSample result)
{
    return FBZZ_WaterMediumBodySample(bodyIndex, worldPos, context, testExclusions,
        allowCausticsOnly, false, result);
}

/// @brief Find the highest-priority optical medium covering a point using one validated query context.
bool FBZZ_QueryWaterMediumWithContext(float3 worldPos, FBZZWaterMediumQueryContext context,
                                     bool testExclusions, bool allowCausticsOnly, bool receiverBoundary,
                                     out FBZZWaterMediumSample result)
{
    result = (FBZZWaterMediumSample)0;
    if (context.bodyCount == 0u || context.tableRows < FBZZ_WATER_MEDIUM_HEADER_ROWS) return false;
    const float4 bounds = FBZZ_WaterMediumRow(1u);
    float2 vertical;
    if (FBZZ_WaterMediumVerticalBounds(allowCausticsOnly, vertical)) {
        if (worldPos.y < vertical.x || worldPos.y > vertical.y) return false;
    }
    const float2 span = bounds.zw - bounds.xy;
    if (any(worldPos.xz < bounds.xy) || any(worldPos.xz > bounds.zw) || any(span <= 1.0e-5f)) return false;
    const uint2 cell = min((uint2)(saturate((worldPos.xz - bounds.xy) / span) * FBZZ_WATER_MEDIUM_GRID_RESOLUTION),
                           FBZZ_WATER_MEDIUM_GRID_RESOLUTION - 1u);
    const uint2 gridSize = (uint2)round(FBZZ_WaterMediumRow(2u).xy);
    const uint cellStart = (uint)round(FBZZ_WaterMediumRow(2u).z);
    const uint candidateStart = (uint)round(FBZZ_WaterMediumRow(3u).w);
    if (any(gridSize == 0u)) return false;
    const uint cellIndex = cell.y * gridSize.x + cell.x;
    const uint cellRow = cellStart + cellIndex;
    if (cellRow >= context.tableRows) return false;
    const float4 cellHeader = FBZZ_WaterMediumRow(cellRow);
    const uint firstCandidate = (uint)round(cellHeader.x);
    const uint candidateCount = min((uint)round(cellHeader.y), 256u);
    /// @note A cell with one eligible candidate has no priority competition; keep malformed-priority tables on the general path.
    if (candidateCount == 1u && candidateStart + firstCandidate < context.tableRows) {
        const uint body = (uint)round(FBZZ_WaterMediumRow(candidateStart + firstCandidate).x);
        if (body < context.bodyCount) {
            const float priority = FBZZ_WaterMediumRow(FBZZ_WATER_MEDIUM_HEADER_ROWS
                + body * FBZZ_WATER_MEDIUM_BODY_ROWS + 11u).x;
            if (priority > -3.402823466e+38f)
                return FBZZ_WaterMediumBodySample(body, worldPos, context, testExclusions, allowCausticsOnly, receiverBoundary, result);
        }
    }
    uint selectedBody = 0xFFFFFFFFu;
    float selectedPriority = -3.402823466e+38f;
    float selectedSurfaceY = -3.402823466e+38f;
    [loop]
    for (uint i = 0u; i < candidateCount; ++i)
    {
        const uint candidateRow = candidateStart + firstCandidate + i;
        if (candidateRow >= context.tableRows) break;
        const uint bodyIndex = (uint)round(FBZZ_WaterMediumRow(candidateRow).x);
        if (bodyIndex >= context.bodyCount) continue;
        const uint bodyOffset = FBZZ_WATER_MEDIUM_HEADER_ROWS + bodyIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
        const float4 flags = FBZZ_WaterMediumRow(bodyOffset + 11u);
        if (flags.y < 0.5f && (!allowCausticsOnly || flags.z < 0.5f)) continue;
        FBZZWaterMediumSample candidate;
        if (!FBZZ_WaterMediumBodySample(bodyIndex, worldPos, context, testExclusions,
                                        allowCausticsOnly, receiverBoundary, candidate)) continue;
        const bool betterPriority = flags.x > selectedPriority;
        const bool equalPriorityHigherSurface = flags.x == selectedPriority
            && candidate.surfaceY > selectedSurfaceY;
        const bool equalPriorityStableKey = flags.x == selectedPriority
            && candidate.surfaceY == selectedSurfaceY && bodyIndex < selectedBody;
        if (betterPriority || equalPriorityHigherSurface || equalPriorityStableKey)
        {
            selectedBody = bodyIndex;
            selectedPriority = flags.x;
            selectedSurfaceY = candidate.surfaceY;
            result = candidate;
        }
    }
    return selectedBody != 0xFFFFFFFFu;
}

bool FBZZ_QueryWaterMediumWithContext(float3 worldPos, FBZZWaterMediumQueryContext context,
                                     bool testExclusions, bool allowCausticsOnly,
                                     out FBZZWaterMediumSample result)
{
    return FBZZ_QueryWaterMediumWithContext(worldPos, context, testExclusions, allowCausticsOnly, false, result);
}

/// @brief Resolve surface lighting at a Terrain bottom within its sampled footprint error, retaining owners, boxes and depth limits.
bool FBZZ_QueryWaterReceiverMediumWithContext(float3 worldPos, FBZZWaterMediumQueryContext context,
                                     out FBZZWaterMediumSample result)
{
    return FBZZ_QueryWaterMediumWithContext(worldPos, context, true, false, true, result);
}

bool FBZZ_QueryWaterReceiverMedium(float3 worldPos, out FBZZWaterMediumSample result)
{
    result = (FBZZWaterMediumSample)0;
    FBZZWaterMediumQueryContext context;
    if (!FBZZ_WaterMediumHeader(context)) return false;
    return FBZZ_QueryWaterReceiverMediumWithContext(worldPos, context, result);
}

bool FBZZ_QueryWaterMediumWithContext(float3 worldPos, FBZZWaterMediumQueryContext context,
                                     bool testExclusions, out FBZZWaterMediumSample result)
{
    return FBZZ_QueryWaterMediumWithContext(worldPos, context, testExclusions, false, result);
}

bool FBZZ_QueryWaterMediumWithContext(float3 worldPos, FBZZWaterMediumQueryContext context,
                                     out FBZZWaterMediumSample result)
{
    return FBZZ_QueryWaterMediumWithContext(worldPos, context, true, false, result);
}

/// @brief Find the highest-priority optical medium covering a world-space point.
bool FBZZ_QueryWaterMedium(float3 worldPos, out FBZZWaterMediumSample result)
{
    result = (FBZZWaterMediumSample)0;
    FBZZWaterMediumQueryContext context;
    if (!FBZZ_WaterMediumHeader(context)) return false;
    return FBZZ_QueryWaterMediumWithContext(worldPos, context, result);
}

/// @brief Clip a parametric ray interval against one axis-aligned slab.
bool FBZZ_WaterClipSlab(float origin, float direction, float minimum, float maximum,
                        inout float tMinimum, inout float tMaximum)
{
    if (abs(direction) <= 1.0e-8f) return origin >= minimum && origin <= maximum;
    const float first = (minimum - origin) / direction;
    const float second = (maximum - origin) / direction;
    tMinimum = max(tMinimum, min(first, second));
    tMaximum = min(tMaximum, max(first, second));
    return tMinimum <= tMaximum;
}

/// @brief Clip a ray against one transformed exclusion box.
bool FBZZ_WaterClipExclusion(uint row, float3 origin, float3 direction,
                             float intervalMinimum, float intervalMaximum,
                             FBZZWaterMediumQueryContext context, out float tMinimum, out float tMaximum)
{
    tMinimum = intervalMinimum;
    tMaximum = intervalMaximum;
    if (row + 3u >= context.tableRows) return false;
    const float4 x = FBZZ_WaterMediumRow(row);
    const float4 y = FBZZ_WaterMediumRow(row + 1u);
    const float4 z = FBZZ_WaterMediumRow(row + 2u);
    const float3 extent = max(FBZZ_WaterMediumRow(row + 3u).xyz, 0.0f);
    const float4 origin4 = float4(origin, 1.0f);
    const float3 localOrigin = float3(dot(x, origin4), dot(y, origin4), dot(z, origin4));
    const float3 localDirection = float3(dot(x.xyz, direction), dot(y.xyz, direction), dot(z.xyz, direction));
    return FBZZ_WaterClipSlab(localOrigin.x, localDirection.x, -extent.x, extent.x, tMinimum, tMaximum)
        && FBZZ_WaterClipSlab(localOrigin.y, localDirection.y, -extent.y, extent.y, tMinimum, tMaximum)
        && FBZZ_WaterClipSlab(localOrigin.z, localDirection.z, -extent.z, extent.z, tMinimum, tMaximum);
}

/// @brief Merge intersected dry-box intervals and return only their complement's ray length.
float FBZZ_WaterUnexcludedDistance(uint bodyIndex, float3 origin, float3 direction,
                                   float intervalMinimum, float intervalMaximum,
                                   FBZZWaterMediumQueryContext context)
{
    if (intervalMaximum <= intervalMinimum) return 0.0f;
    const uint bodyOffset = FBZZ_WATER_MEDIUM_HEADER_ROWS + bodyIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
    const float4 spans = FBZZ_WaterMediumRow(bodyOffset + 10u);
    const uint first = (uint)round(spans.x);
    const uint count = (uint)round(spans.y);
    if (count == 0u) return intervalMaximum - intervalMinimum;
    const uint exclusionStart = (uint)round(FBZZ_WaterMediumRow(3u).y);
    /// @note A single dry interval needs no union scan; preserve the general path's invalid-table suppression.
    if (count == 1u) {
        const uint row = exclusionStart + first * 4u;
        if (row + 3u >= context.tableRows) return 0.0f;
        float dryStart;
        float dryEnd;
        if (!FBZZ_WaterClipExclusion(row, origin, direction, intervalMinimum, intervalMaximum,
            context, dryStart, dryEnd)) return intervalMaximum - intervalMinimum;
        return max(dryStart - intervalMinimum, 0.0f) + max(intervalMaximum - dryEnd, 0.0f);
    }
    float cursor = intervalMinimum;
    float wetDistance = 0.0f;
    [loop]
    for (uint interval = 0u; interval <= count * 2u && cursor < intervalMaximum; ++interval)
    {
        float nearestDryStart = intervalMaximum;
        float coveringDryEnd = cursor;
        [loop]
        for (uint i = 0u; i < count; ++i)
        {
            const uint row = exclusionStart + (first + i) * 4u;
            if (row + 3u >= context.tableRows) return 0.0f;
            float dryStart;
            float dryEnd;
            if (!FBZZ_WaterClipExclusion(row, origin, direction, intervalMinimum, intervalMaximum,
                                         context, dryStart, dryEnd)) continue;
            if (dryEnd <= cursor || dryStart >= intervalMaximum) continue;
            if (dryStart <= cursor) coveringDryEnd = max(coveringDryEnd, dryEnd);
            else nearestDryStart = min(nearestDryStart, dryStart);
        }
        if (coveringDryEnd > cursor) cursor = min(coveringDryEnd, intervalMaximum);
        else { wetDistance += max(nearestDryStart - cursor, 0.0f); cursor = nearestDryStart; }
    }
    return wetDistance;
}

/// @brief Clip one interval against a rectangular owner's footprint and constant-depth water column.
bool FBZZ_WaterRectangleClipWetInterval(uint bodyIndex, float3 origin, float3 direction,
                                        FBZZWaterMediumQueryContext context,
                                        FBZZWaterMediumSample initial,
                                        inout float intervalMinimum, inout float intervalMaximum)
{
    const uint bodyOffset = FBZZ_WATER_MEDIUM_HEADER_ROWS + bodyIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
    const float2 halfExtent = max(FBZZ_WaterMediumRow(bodyOffset).xy, 0.0f);
    if (any(halfExtent <= 0.0f)) return false;
    const float4 rowX = FBZZ_WaterMediumRow(bodyOffset + 1u);
    const float4 rowY = FBZZ_WaterMediumRow(bodyOffset + 2u);
    const float4 rowZ = FBZZ_WaterMediumRow(bodyOffset + 3u);
    const float4 origin4 = float4(origin, 1.0f);
    const float3 localOrigin = float3(dot(rowX, origin4), dot(rowY, origin4), dot(rowZ, origin4));
    const float3 localDirection = float3(dot(rowX.xyz, direction), dot(rowY.xyz, direction), dot(rowZ.xyz, direction));
    const float waterDepth = min(initial.depth, max(FBZZ_WaterMediumRow(bodyOffset + 6u).z, 0.0f));
    /// @note Rectangle mode 2 is Horizon: the view atlas bounds the work, while horizontal membership is unbounded.
    const bool horizon = FBZZ_WaterMediumRow(bodyOffset + 11u).w > 1.5f;
    if ((!horizon && (!FBZZ_WaterClipSlab(localOrigin.x, localDirection.x, -halfExtent.x, halfExtent.x,
                            intervalMinimum, intervalMaximum)
        || !FBZZ_WaterClipSlab(localOrigin.z, localDirection.z, -halfExtent.y, halfExtent.y,
                               intervalMinimum, intervalMaximum)))
        || !FBZZ_WaterClipSlab(origin.y, direction.y, initial.surfaceY - waterDepth, initial.surfaceY,
                                intervalMinimum, intervalMaximum)) return false;
    intervalMinimum = max(intervalMinimum, 0.0f);
    return intervalMaximum > intervalMinimum;
}

/// @brief Measure the non-excluded length of a rectangular owner's clipped ray interval.
float FBZZ_WaterRectangleWetDistance(uint bodyIndex, float3 origin, float3 direction, float distance,
                                     FBZZWaterMediumQueryContext context, FBZZWaterMediumSample initial)
{
    float intervalMinimum = 0.0f;
    float intervalMaximum = distance;
    if (!FBZZ_WaterRectangleClipWetInterval(bodyIndex, origin, direction, context, initial,
                                            intervalMinimum, intervalMaximum)) return 0.0f;
    const uint bodyOffset = FBZZ_WATER_MEDIUM_HEADER_ROWS + bodyIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
    const float4 spans = FBZZ_WaterMediumRow(bodyOffset + 10u);
    const uint first = (uint)round(spans.x);
    const uint count = (uint)round(spans.y);
    if (count == 0u) return intervalMaximum - intervalMinimum;
    const uint exclusionStart = (uint)round(FBZZ_WaterMediumRow(3u).y);
    float cursor = intervalMinimum;
    float wetDistance = 0.0f;
    [loop]
    for (uint interval = 0u; interval <= count * 2u && cursor < intervalMaximum; ++interval)
    {
        float nearestDryStart = intervalMaximum;
        float coveringDryEnd = cursor;
        [loop]
        for (uint i = 0u; i < count; ++i)
        {
            const uint row = exclusionStart + (first + i) * 4u;
            float dryStart;
            float dryEnd;
            if (!FBZZ_WaterClipExclusion(row, origin, direction, intervalMinimum, intervalMaximum,
                                         context, dryStart, dryEnd)) continue;
            if (dryEnd <= cursor || dryStart >= intervalMaximum) continue;
            if (dryStart <= cursor) coveringDryEnd = max(coveringDryEnd, dryEnd);
            else nearestDryStart = min(nearestDryStart, dryStart);
        }
        if (coveringDryEnd > cursor)
        {
            cursor = min(coveringDryEnd, intervalMaximum);
        }
        else
        {
            wetDistance += max(nearestDryStart - cursor, 0.0f);
            cursor = nearestDryStart;
        }
    }
    return wetDistance;
}

/// @brief Integrate plume extinction at wet subinterval midpoints within one bounded ray cell.
/// @note Dry subintervals do not contribute; partial cells retain every contiguous wet segment.
float3 FBZZ_WaterRectanglePlumeCellOpticalDepth(uint bodyIndex, float3 origin, float3 direction,
                                                float cellStart, float cellEnd,
                                                FBZZWaterMediumQueryContext context,
                                                FBZZWaterMediumSample initial,
                                                float4 sphere, float4 density)
{
    float intervalMinimum = cellStart;
    float intervalMaximum = cellEnd;
    if (!FBZZ_WaterRectangleClipWetInterval(bodyIndex, origin, direction, context, initial,
                                            intervalMinimum, intervalMaximum)) return 0.0f;
    const uint bodyOffset = FBZZ_WATER_MEDIUM_HEADER_ROWS + bodyIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
    const float4 spans = FBZZ_WaterMediumRow(bodyOffset + 10u);
    const uint first = (uint)round(spans.x);
    const uint count = (uint)round(spans.y);
    const uint exclusionStart = (uint)round(FBZZ_WaterMediumRow(3u).y);
    float cursor = intervalMinimum;
    float3 opticalDepth = 0.0f;
    [loop]
    for (uint interval = 0u; interval <= count * 2u && cursor < intervalMaximum; ++interval)
    {
        float nearestDryStart = intervalMaximum;
        float coveringDryEnd = cursor;
        [loop]
        for (uint i = 0u; i < count; ++i)
        {
            const uint row = exclusionStart + (first + i) * 4u;
            if (row + 3u >= context.tableRows) return 0.0f;
            float dryStart;
            float dryEnd;
            if (!FBZZ_WaterClipExclusion(row, origin, direction, intervalMinimum, intervalMaximum,
                                         context, dryStart, dryEnd)) continue;
            if (dryEnd <= cursor || dryStart >= intervalMaximum) continue;
            if (dryStart <= cursor) coveringDryEnd = max(coveringDryEnd, dryEnd);
            else nearestDryStart = min(nearestDryStart, dryStart);
        }
        if (coveringDryEnd > cursor)
        {
            cursor = min(coveringDryEnd, intervalMaximum);
        }
        else
        {
            const float wetEnd = min(nearestDryStart, intervalMaximum);
            if (wetEnd > cursor)
            {
                const float midpointDistance = (cursor + wetEnd) * 0.5f;
                const float3 midpoint = origin + direction * midpointDistance;
                const float falloff = saturate(1.0f - length(midpoint - sphere.xyz) / max(sphere.w, 1.0e-4f));
                opticalDepth += max(density.rgb, 0.0f) * max(density.w, 0.0f)
                    * falloff * falloff * (wetEnd - cursor);
            }
            cursor = nearestDryStart;
        }
    }
    return opticalDepth;
}

/// @brief Integrate one bounded source interval, subtracting complete dry-box subintervals from each cell.
float3 FBZZ_WaterSourceOpticalDepth(float3 origin, float3 direction, float start, float end,
                                    FBZZWaterMediumQueryContext context, uint initialOwner,
                                    bool allowCausticsOnly)
{
    if (end <= start) return 0.0f;
    const uint steps = (uint)clamp(ceil((end - start) / 16.0f), 4.0f, 8.0f);
    float3 opticalDepth = 0.0f;
    [loop]
    for (uint i = 0u; i < steps; ++i)
    {
        float cellStart = lerp(start, end, (float)i / (float)steps);
        float cellEnd = lerp(start, end, (float)(i + 1u) / (float)steps);
        const float3 midpoint = origin + direction * ((cellStart + cellEnd) * 0.5f);
        FBZZWaterMediumSample medium;
        bool found = FBZZ_QueryWaterMediumWithContext(midpoint, context, false, medium);
        if (!found && allowCausticsOnly)
            found = FBZZ_WaterMediumBodySample(initialOwner, midpoint, context, false, true, medium);
        if (!found) continue;
        const uint offset = FBZZ_WATER_MEDIUM_HEADER_ROWS + medium.ownerIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
        const float4 bounds = FBZZ_WaterMediumRow(offset + 9u);
        if (!FBZZ_WaterClipSlab(origin.x, direction.x, bounds.x, bounds.z, cellStart, cellEnd)
            || !FBZZ_WaterClipSlab(origin.z, direction.z, bounds.y, bounds.w, cellStart, cellEnd)
            || !FBZZ_WaterClipSlab(origin.y, direction.y, medium.surfaceY - medium.depth,
                                   medium.surfaceY, cellStart, cellEnd)) continue;
        const float wetDistance = FBZZ_WaterUnexcludedDistance(medium.ownerIndex, origin, direction,
            cellStart, cellEnd, context);
        const bool dryMidpoint = FBZZ_WaterPointExcluded(medium.ownerIndex, midpoint, context);
        const float4 flags = FBZZ_WaterMediumRow(offset + 11u);
        const float3 extinction = flags.y > 0.5f
            ? medium.absorption + medium.scattering + (dryMidpoint ? 0.0f : medium.plumeScattering)
            : float3(0.45f, 0.45f, 0.45f);
        opticalDepth += extinction * wetDistance;
    }
    return opticalDepth;
}

/// @brief Integrate compact plume support without re-querying a homogeneous rectangular water atlas.
float3 FBZZ_WaterRectanglePlumeOpticalDepth(float3 origin, float3 direction, float distance,
                                            FBZZWaterMediumQueryContext context, FBZZWaterMediumSample initial)
{
    const uint offset = FBZZ_WATER_MEDIUM_HEADER_ROWS + initial.ownerIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
    const float4 spans = FBZZ_WaterMediumRow(offset + 10u);
    const uint first = (uint)round(spans.z);
    const uint count = (uint)round(spans.w);
    const uint plumeStart = (uint)round(FBZZ_WaterMediumRow(3u).z);
    float3 opticalDepth = 0.0f;
    [loop]
    for (uint i = 0u; i < count; ++i)
    {
        const uint row = plumeStart + (first + i) * 2u;
        if (row + 1u >= context.tableRows) break;
        const float4 sphere = FBZZ_WaterMediumRow(row);
        const float4 density = FBZZ_WaterMediumRow(row + 1u);
        if (sphere.w <= 0.0f || density.w <= 0.0f || all(density.rgb <= 0.0f)) continue;
        const float3 toCenter = sphere.xyz - origin;
        const float centerT = dot(toCenter, direction);
        const float perpendicularSq = max(dot(toCenter, toCenter) - centerT * centerT, 0.0f);
        const float chordSq = sphere.w * sphere.w - perpendicularSq;
        if (chordSq <= 0.0f) continue;
        const float halfChord = sqrt(chordSq);
        const float start = max(centerT - halfChord, 0.0f);
        const float end = min(centerT + halfChord, distance);
        if (end <= start) continue;
        const float cellLength = (end - start) * 0.25f;
        [unroll]
        for (uint cell = 0u; cell < 4u; ++cell)
        {
            const float cellStart = start + (float)cell * cellLength;
            opticalDepth += FBZZ_WaterRectanglePlumeCellOpticalDepth(initial.ownerIndex,
                origin, direction, cellStart, cellStart + cellLength, context, initial, sphere, density);
        }
    }
    return opticalDepth;
}

/// @brief Reject other owner footprints before using a homogeneous owner-specific path.
bool FBZZ_WaterRayOwnerIsolated(uint owner, float3 origin, float3 direction, float distance,
                               FBZZWaterMediumQueryContext context)
{
    [loop] for (uint body = 0u; body < context.bodyCount; ++body) {
        if (body == owner) continue;
        const float4 bounds = FBZZ_WaterMediumRow(FBZZ_WATER_MEDIUM_HEADER_ROWS
            + body * FBZZ_WATER_MEDIUM_BODY_ROWS + 9u);
        float first = 0.0f;
        float last = distance;
        if (FBZZ_WaterClipSlab(origin.x, direction.x, bounds.x, bounds.z, first, last)
            && FBZZ_WaterClipSlab(origin.z, direction.z, bounds.y, bounds.w, first, last)) return false;
    }
    return true;
}

/// @brief Prove a whole segment lies below every possible wave and above every constant-depth bottom.
/// @note Dry boxes are retained by exact interval subtraction; plumes and variable-depth owners use the general integrator.
/// @see https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance Homogeneous Beer attenuation.
bool FBZZ_WaterCertifiedSegment(uint owner, float3 origin, float3 direction, float distance,
                               FBZZWaterMediumQueryContext context)
{
    const uint offset = FBZZ_WATER_MEDIUM_HEADER_ROWS + owner * FBZZ_WATER_MEDIUM_BODY_ROWS;
    const float4 geometry = FBZZ_WaterMediumRow(offset);
    const float4 flags = FBZZ_WaterMediumRow(offset + 11u);
    const float4 spans = FBZZ_WaterMediumRow(offset + 10u);
    if (geometry.w < 1.0f || geometry.z > 0.5f || flags.w < 0.5f || spans.w > 0.0f) return false;
    const float4 limits = FBZZ_WaterMediumRow(offset + 6u);
    const float margin = geometry.w - 1.0f;
    const float3 end = origin + direction * distance;
    if (min(origin.y, end.y) <= limits.x + margin - limits.z
        || max(origin.y, end.y) >= limits.x - margin) return false;
    const float4 x = FBZZ_WaterMediumRow(offset + 1u);
    const float4 z = FBZZ_WaterMediumRow(offset + 3u);
    if (abs(dot(x, float4(origin, 1.0f))) >= geometry.x
        || abs(dot(x, float4(end, 1.0f))) >= geometry.x
        || abs(dot(z, float4(origin, 1.0f))) >= geometry.y
        || abs(dot(z, float4(end, 1.0f))) >= geometry.y) return false;
    return FBZZ_WaterRayOwnerIsolated(owner, origin, direction, distance, context);
}

/// @brief Prove the resolved vertical column keeps ownership until its surface and has no water beyond it.
/// @note Compare the same priority, sampled height, and stable index as point queries; dry boxes do not transfer ownership.
bool FBZZ_WaterVerticalSourceOwnerDominates(float3 origin, float distance,
                                           FBZZWaterMediumQueryContext context,
                                           FBZZWaterMediumSample initial)
{
    const uint initialOffset = FBZZ_WATER_MEDIUM_HEADER_ROWS + initial.ownerIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
    const float4 initialFlags = FBZZ_WaterMediumRow(initialOffset + 11u);
    const float rayEndY = origin.y + distance;
    [loop]
    for (uint body = 0u; body < context.bodyCount; ++body)
    {
        if (body == initial.ownerIndex) continue;
        const uint offset = FBZZ_WATER_MEDIUM_HEADER_ROWS + body * FBZZ_WATER_MEDIUM_BODY_ROWS;
        const float4 flags = FBZZ_WaterMediumRow(offset + 11u);
        if (flags.y < 0.5f) continue;
        const float4 limits = FBZZ_WaterMediumRow(offset + 6u);
        float4 atlas;
        if (!FBZZ_WaterAtlasSample(body, origin.xz, context, atlas)) continue;
        const float surface = limits.x + atlas.x;
        const float depth = min(max(atlas.y, 0.0f), max(limits.z, 0.0f));
        const float bottom = surface - depth;
        if (depth <= 0.0f || surface <= origin.y || bottom >= rayEndY) continue;
        /// @note After the initial surface, even a lower-priority column may attenuate a source above the water.
        if (surface > initial.surfaceY && rayEndY > max(bottom, initial.surfaceY)) return false;
        const bool wins = initialFlags.y < 0.5f || flags.x > initialFlags.x
            || (flags.x == initialFlags.x && (surface > initial.surfaceY
                || (surface == initial.surfaceY && body < initial.ownerIndex)));
        if (wins) return false;
    }
    return true;
}

/// @brief Reuse a resolved receiver and integrate source Beer with exact boxes and a bounded wave-boundary search.
/// @see https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance PBRT, optical-depth integration.
float3 FBZZ_WaterSourceRGBTransmittanceInternal(float3 origin, float3 towardSource,
                                               float maxSourceDistance,
                                               FBZZWaterMediumQueryContext context,
                                               FBZZWaterMediumSample initial, bool allowCausticsOnly)
{
    const float directionLengthSq = dot(towardSource, towardSource);
    if (directionLengthSq <= 1.0e-8f || maxSourceDistance <= 0.0f
        || initial.ownerIndex >= context.bodyCount) return 1.0f;
    const float3 direction = towardSource * rsqrt(directionLengthSq);
    const float distance = min(maxSourceDistance, initial.maxDistance);
    if (distance <= 1.0e-6f) return 1.0f;
    const uint bodyOffset = FBZZ_WATER_MEDIUM_HEADER_ROWS + initial.ownerIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
    const float4 flags = FBZZ_WaterMediumRow(bodyOffset + 11u);
    const float4 spans = FBZZ_WaterMediumRow(bodyOffset + 10u);
    [branch]
    if (FBZZ_WaterCertifiedSegment(initial.ownerIndex, origin, direction, distance, context)) {
        const float wetDistance = FBZZ_WaterUnexcludedDistance(initial.ownerIndex, origin, direction,
            0.0f, distance, context);
        const float3 extinction = flags.y > 0.5f ? initial.absorption + initial.scattering
            : float3(0.45f, 0.45f, 0.45f);
        return exp(-min(extinction * wetDistance, 80.0f));
    }
    if (flags.w > 0.5f && context.bodyCount == 1u
        && FBZZ_WaterMediumRow(bodyOffset).z < 0.5f)
    {
        const float wetDistance = FBZZ_WaterRectangleWetDistance(initial.ownerIndex, origin, direction,
            distance, context, initial);
        const float3 extinction = flags.y > 0.5f ? initial.absorption + initial.scattering
            : float3(0.45f, 0.45f, 0.45f);
        const float3 plumeDepth = flags.y > 0.5f && (uint)round(spans.w) > 0u
            ? FBZZ_WaterRectanglePlumeOpticalDepth(origin, direction, distance, context, initial) : 0.0f;
        return exp(-min(extinction * wetDistance + plumeDepth, 80.0f));
    }

    /// @note A vertical ray keeps the same atlas column even for waves and terrain depth; dry boxes still remove exact intervals.
    /// @see https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance PBRT v4, §11.2 homogeneous Beer law.
    [branch]
    if (all(direction.xz == 0.0f) && direction.y > 0.0f && (uint)round(spans.w) == 0u
        && FBZZ_WaterVerticalSourceOwnerDominates(origin, distance, context, initial))
    {
            const float columnDistance = min(distance, max(initial.surfaceY - origin.y, 0.0f));
            const float wetDistance = FBZZ_WaterUnexcludedDistance(initial.ownerIndex, origin, direction,
                0.0f, columnDistance, context);
            const float3 extinction = flags.y > 0.5f ? initial.absorption + initial.scattering
                : float3(0.45f, 0.45f, 0.45f);
            return exp(-min(extinction * wetDistance, 80.0f));
    }

    /// @note Seed from the nearby surface/bottom, so a two-metre Sun path is not skipped by a 120-metre march.
    float estimate = distance;
    if (abs(direction.y) > 1.0e-4f)
    {
        const float boundaryY = direction.y > 0.0f ? initial.surfaceY : initial.surfaceY - initial.depth;
        estimate = min(estimate, max((boundaryY - origin.y) / direction.y, 0.0f));
    }
    const float4 bounds = FBZZ_WaterMediumRow(bodyOffset + 9u);
    float boundsStart = 0.0f;
    float boundsEnd = distance;
    if (FBZZ_WaterClipSlab(origin.x, direction.x, bounds.x, bounds.z, boundsStart, boundsEnd)
        && FBZZ_WaterClipSlab(origin.z, direction.z, bounds.y, bounds.w, boundsStart, boundsEnd))
        estimate = min(estimate, boundsEnd);
    const float firstProbe = min(max(estimate + 0.01f, 0.01f), distance);
    float firstExit = distance;
    float previousInside = 0.0f;
    bool foundExit = false;
    [loop]
    for (uint probe = 0u; probe < 8u; ++probe)
    {
        const float probeDistance = min(firstProbe * exp2((float)probe), distance);
        FBZZWaterMediumSample probeSample;
        if (!FBZZ_WaterMediumBodySample(initial.ownerIndex, origin + direction * probeDistance,
                                        context, false, allowCausticsOnly, probeSample))
        {
            float outside = probeDistance;
            float inside = previousInside;
            [unroll]
            for (uint search = 0u; search < 8u; ++search)
            {
                const float midpoint = (inside + outside) * 0.5f;
                FBZZWaterMediumSample midpointSample;
                if (FBZZ_WaterMediumBodySample(initial.ownerIndex, origin + direction * midpoint,
                                               context, false, allowCausticsOnly, midpointSample)) inside = midpoint;
                else outside = midpoint;
            }
            firstExit = (inside + outside) * 0.5f;
            foundExit = true;
            break;
        }
        previousInside = probeDistance;
        if (probeDistance >= distance) break;
    }
    float3 opticalDepth = FBZZ_WaterSourceOpticalDepth(origin, direction, 0.0f, firstExit,
        context, initial.ownerIndex, allowCausticsOnly);
    if (foundExit && firstExit < distance && context.bodyCount > 1u)
        opticalDepth += FBZZ_WaterSourceOpticalDepth(origin, direction, firstExit, distance,
            context, initial.ownerIndex, allowCausticsOnly);
    return exp(-min(opticalDepth, 80.0f));
}

/// @brief Compute finite RGB transmittance for an optical water source path.
float3 FBZZ_WaterSourceRGBTransmittance(float3 origin, float3 towardSource,
                                       float maxSourceDistance, uint ownerIndex)
{
    FBZZWaterMediumQueryContext context;
    FBZZWaterMediumSample initial;
    if (!FBZZ_WaterMediumHeader(context)
        || !FBZZ_WaterMediumBodySample(ownerIndex, origin, context, true, false, initial)) return 1.0f;
    return FBZZ_WaterSourceRGBTransmittanceInternal(origin, towardSource, maxSourceDistance,
        context, initial, false);
}

/// @brief Cached source query for already-resolved volume and receiver samples.
float3 FBZZ_WaterSourceRGBTransmittance(float3 origin, float3 towardSource, float maxSourceDistance,
                                       FBZZWaterMediumQueryContext context, FBZZWaterMediumSample initial)
{
    return FBZZ_WaterSourceRGBTransmittanceInternal(origin, towardSource, maxSourceDistance,
        context, initial, false);
}

float3 FBZZ_WaterSourceRGBTransmittance(float3 origin, float3 towardSource, float maxSourceDistance,
                                       FBZZWaterMediumSample initial)
{
    FBZZWaterMediumQueryContext context;
    if (!FBZZ_WaterMediumHeader(context)) return 1.0f;
    return FBZZ_WaterSourceRGBTransmittance(origin, towardSource, maxSourceDistance, context, initial);
}

/// @brief Compute finite RGB transmittance for caustics, including legacy absorption on non-optical water.
float3 FBZZ_WaterCausticSourceRGBTransmittance(float3 origin, float3 towardSource,
                                               float maxSourceDistance, uint ownerIndex)
{
    FBZZWaterMediumQueryContext context;
    FBZZWaterMediumSample initial;
    if (!FBZZ_WaterMediumHeader(context)
        || !FBZZ_WaterMediumBodySample(ownerIndex, origin, context, true, true, initial)) return 1.0f;
    return FBZZ_WaterSourceRGBTransmittanceInternal(origin, towardSource, maxSourceDistance,
        context, initial, true);
}

/// @brief Convert the atlas Jacobian into bounded surface compression for caustic concentration.
float FBZZ_WaterCompression(FBZZWaterMediumSample medium)
{
    return saturate(1.0f - medium.jacobian);
}

#endif
