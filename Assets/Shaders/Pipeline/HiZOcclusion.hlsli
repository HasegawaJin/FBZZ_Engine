/// @file    HiZOcclusion.hlsli
/// @brief   Reversed-Z 階層から現在ビューの遮蔽を判定する。
/// @author  Hasegawa Jin
/// @date    2026-11-12
#ifndef FBZZ_HIZ_OCCLUSION_HLSLI
#define FBZZ_HIZ_OCCLUSION_HLSLI

/// @brief Tests all tiles covering a projected sphere against the current reversed-Z depth hierarchy.
/// @note Bounds are expanded for clip-space arithmetic and raster sample positions. Near and viewport intersections keep the candidate.
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-v-image-oriented-computing/chapter-42-conservative-rasterization Conservative Depth.
bool OccludedByHiZ(float3 center, float radius, float4x4 viewProjection, uint2 size,
    StructuredBuffer<float> hierarchy)
{
    if (radius <= 0 || !all(isfinite(float4(center, radius)))) return false;
    const float4 position = float4(center, 1);
    const float4 clipCenter = mul(position, viewProjection);
    const float4 clipX = radius * viewProjection[0];
    const float4 clipY = radius * viewProjection[1];
    const float4 clipZ = radius * viewProjection[2];
    const float4 error = 2.0e-6 * (mul(abs(position), abs(viewProjection))
        + abs(clipX) + abs(clipY) + abs(clipZ)) + 1.0e-7;
    if (!all(isfinite(error))) return false;

    float2 low = 1.0, high = -1.0;
    float nearest = 0.0;
    [unroll] for (uint corner = 0; corner < 8; ++corner) {
        const float4 clip = clipCenter + ((corner & 1) ? clipX : -clipX)
            + ((corner & 2) ? clipY : -clipY) + ((corner & 4) ? clipZ : -clipZ);
        const float minW = clip.w - error.w;
        const float maxZ = clip.z + error.z;
        if (!all(isfinite(clip)) || minW <= 0 || clip.z - error.z <= 0 || maxZ >= minW) return false;
        const float2 ndc = clip.xy / clip.w;
        const float2 margin = (error.xy + abs(ndc) * error.w) / minW;
        low = min(low, ndc - margin);
        high = max(high, ndc + margin);
        nearest = max(nearest, maxZ / minW);
    }

    const float2 dimensions = float2(size);
    const float2 firstPixel = float2(low.x * 0.5 + 0.5, 0.5 - high.y * 0.5) * dimensions - 1.0;
    const float2 lastPixel = float2(high.x * 0.5 + 0.5, 0.5 - low.y * 0.5) * dimensions + 1.0;
    if (!all(isfinite(float4(firstPixel, lastPixel))) || any(firstPixel < 0) || any(lastPixel >= dimensions)) return false;

    /// @note Levels are tightly packed, starting at ceil(viewport/4) with ceil-halving to 1×1.
    uint2 tiles = (size + 3u) / 4u;
    uint2 first = uint2(floor(firstPixel)) / 4u;
    uint2 last = uint2(floor(lastPixel)) / 4u;
    uint levelOffset = 0u;
    [loop] while (any(last - first > 1u)) {
        levelOffset += tiles.x * tiles.y;
        tiles = (tiles + 1u) / 2u;
        first /= 2u;
        last /= 2u;
    }
    /// @note Min reduction preserves the farthest covered pixel, including the far clear depth of holes.
    const float guardedNearest = nearest + 1.0e-5;
    [loop] for (uint y = first.y; y <= last.y; ++y) {
        [loop] for (uint x = first.x; x <= last.x; ++x) {
            const float depth = hierarchy[levelOffset + y * tiles.x + x];
            if (!isfinite(depth) || depth <= guardedNearest || depth > 1.0) return false;
        }
    }
    return true;
}

#endif
