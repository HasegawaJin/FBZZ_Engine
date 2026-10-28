/// @file    Underwater.hlsli
/// @brief   Shared water-volume clipping and optical-coordinate helpers.
/// @author  Hasegawa Jin
/// @date    2026-10-09
#ifndef FBZZ_UNDERWATER_HLSLI
#define FBZZ_UNDERWATER_HLSLI

#include "Water/WaterOptics.hlsli"
#include "Rendering/Shadow.hlsli"

/// @brief Samples raw geometry-shadow visibility with tile-clamped four-tap PCF.
/// @param shadowMap Raw shadow depth atlas.
/// @param shadowSampler Comparison sampler for the shadow atlas.
/// @param uv Cascade-local shadow coordinates.
/// @param depth Receiver depth after the caller applies its fixed bias.
/// @param atlasRect Cascade atlas rectangle.
/// @return Fraction of visible taps, independent of decorative shadow strength.
/// @note Each tap is clamped to the cascade tile so linear comparison filtering cannot read an adjacent tile.
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-samplecmp SampleCmp comparison filtering.
float FBZZ_SampleWaterShadowPcf(Texture2D<float> shadowMap,
                                SamplerComparisonState shadowSampler,
                                float2 uv, float depth, float4 atlasRect)
{
    float2 inset = min(shadowMapTexelSize, atlasRect.zw * 0.5f);
    float2 uvMin = atlasRect.xy + inset;
    float2 uvMax = atlasRect.xy + atlasRect.zw - inset;
    float2 atlasUV = CascadeUVToAtlas(uv, atlasRect);
    float visibility = 0.0f;
    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            float2 offset = (float2(x, y) - 0.5f) * shadowMapTexelSize;
            float2 sampleUV = clamp(atlasUV + offset, uvMin, uvMax);
            visibility += shadowMap.SampleCmpLevelZero(shadowSampler, sampleUV, depth);
        }
    }
    return visibility * 0.25f;
}

/// @brief Evaluates raw cascaded geometry-shadow visibility for a water-light sample.
/// @param shadowMap Raw shadow depth atlas.
/// @param shadowSampler Comparison sampler for the shadow atlas.
/// @param worldPos World-space receiver position.
/// @return Geometry visibility; samples outside shadow coverage are treated as lit.
/// @param biasScale One for volume positions; a bounded slope factor for surface receivers.
/// @note Physical visibility ignores decorative shadow strength and retains cascade blending.
float FBZZ_WaterGeometryVisibilityScaled(Texture2D<float> shadowMap,
                                   SamplerComparisonState shadowSampler,
                                    float3 worldPos, float biasScale)
{
    if (cascadeCount <= 1)
    {
        float2 uv;
        float depth;
        WorldToShadowUV(worldPos, lightViewProjection, uv, depth);
        if (any(uv < 0.0f) || any(uv > 1.0f) || depth < 0.0f || depth > 1.0f)
            return 1.0f;
        return FBZZ_SampleWaterShadowPcf(shadowMap, shadowSampler, uv,
            depth - shadowBias * biasScale, float4(0.0f, 0.0f, 1.0f, 1.0f));
    }

    float2 uv;
    float depth;
    float edge;
    int index = SelectShadowCascade(worldPos, uv, depth, edge);
    if (index < 0) return 1.0f;
    float visibility = FBZZ_SampleWaterShadowPcf(shadowMap, shadowSampler, uv,
        depth - CascadeBiasAt(index) * biasScale, cascadeAtlasRect[index]);

    if (cascadeBlend > 0.0f && index + 1 < cascadeCount)
    {
        float blendStart = 1.0f - cascadeBlend;
        if (edge > blendStart)
        {
            float2 nextUV;
            float nextDepth;
            WorldToShadowUV(worldPos, cascadeViewProjection[index + 1], nextUV, nextDepth);
            if (all(nextUV >= 0.0f) && all(nextUV <= 1.0f) && nextDepth >= 0.0f && nextDepth <= 1.0f)
            {
                float nextVisibility = FBZZ_SampleWaterShadowPcf(shadowMap, shadowSampler,
                    nextUV, nextDepth - CascadeBiasAt(index + 1) * biasScale, cascadeAtlasRect[index + 1]);
                float blend = saturate((edge - blendStart) / max(cascadeBlend, 1.0e-4f));
                visibility = lerp(visibility, nextVisibility, blend);
            }
        }
    }
    return visibility;
}

/// @note Volume samples have no receiver plane; they retain the fixed physical visibility bias.
float FBZZ_WaterGeometryVisibility(Texture2D<float> shadowMap,
    SamplerComparisonState shadowSampler, float3 worldPos)
{
    return FBZZ_WaterGeometryVisibilityScaled(shadowMap, shadowSampler, worldPos, 1.0f);
}

/// @brief Transforms world XZ into a yawed water rectangle's local coordinates.
/// @param worldXZ World-space horizontal position.
/// @param center World-space rectangle center.
/// @param cosYaw Cosine of the rectangle yaw.
/// @param sinYaw Sine of the rectangle yaw.
/// @return Coordinates aligned to the water rectangle axes.
float2 FBZZ_UnderwaterWorldToLocalXZ(float2 worldXZ, float2 center, float cosYaw, float sinYaw)
{
    float2 relative = worldXZ - center;
    return float2(cosYaw * relative.x - sinYaw * relative.y,
                  sinYaw * relative.x + cosYaw * relative.y);
}

/// @brief Tests whether a world position lies within an optional yawed water rectangle.
/// @param worldXZ World-space horizontal position.
/// @param center World-space rectangle center.
/// @param halfExtent World-space half extents; a nonpositive axis disables the rectangle clip.
/// @param cosYaw Cosine of the rectangle yaw.
/// @param sinYaw Sine of the rectangle yaw.
/// @return True when the point is inside the rectangle or the rectangle is unbounded.
bool FBZZ_UnderwaterContainsXZ(float2 worldXZ, float2 center, float2 halfExtent,
                               float cosYaw, float sinYaw)
{
    if (halfExtent.x <= 0.0f || halfExtent.y <= 0.0f)
        return true;

    float2 local = FBZZ_UnderwaterWorldToLocalXZ(worldXZ, center, cosYaw, sinYaw);
    return all(abs(local) <= halfExtent);
}

/// @brief Clips a ray interval against one scalar slab.
/// @param origin Ray origin coordinate.
/// @param direction Ray direction coordinate.
/// @param minimum Slab minimum.
/// @param maximum Slab maximum.
/// @param intervalStart Current interval start, updated in place.
/// @param intervalEnd Current interval end, updated in place.
/// @return True when the updated interval remains nonempty.
bool FBZZ_UnderwaterClipAxis(float origin, float direction, float minimum, float maximum,
                             inout float intervalStart, inout float intervalEnd)
{
    if (abs(direction) <= 1.0e-7f)
        return origin >= minimum && origin <= maximum;

    float first = (minimum - origin) / direction;
    float second = (maximum - origin) / direction;
    intervalStart = max(intervalStart, min(first, second));
    intervalEnd = min(intervalEnd, max(first, second));
    return intervalEnd >= intervalStart;
}

/// @brief Clips a world-space ray against the horizontal water slab and optional yawed rectangle.
/// @param origin World-space ray origin.
/// @param direction Normalized world-space ray direction.
/// @param maxDistance Maximum ray distance in meters.
/// @param surfaceY Horizontal water surface height.
/// @param bottomY Horizontal finite water bottom; values far below the scene act as unbounded.
/// @param center World-space XZ center of the optional rectangle.
/// @param halfExtent World-space half extents; a nonpositive axis disables the rectangle clip.
/// @param cosYaw Cosine of the rectangle yaw.
/// @param sinYaw Sine of the rectangle yaw.
/// @param outStart First in-volume ray distance in meters.
/// @param outEnd Last in-volume ray distance in meters.
/// @return True when the ray intersects a nonzero portion of the water volume.
/// @see https://www.pbr-book.org/4ed/Shapes/Basic_Shape_Interface PBRT v4, ray clipping against a bounded volume.
bool FBZZ_ClipUnderwaterRay(float3 origin, float3 direction, float maxDistance,
                            float surfaceY, float bottomY, float2 center, float2 halfExtent,
                            float cosYaw, float sinYaw, out float outStart, out float outEnd)
{
    float intervalStart = 0.0f;
    float intervalEnd = max(maxDistance, 0.0f);
    if (intervalEnd <= 0.0f || surfaceY <= bottomY)
    {
        outStart = 0.0f;
        outEnd = 0.0f;
        return false;
    }

    if (!FBZZ_UnderwaterClipAxis(origin.y, direction.y, bottomY, surfaceY,
                                 intervalStart, intervalEnd))
    {
        outStart = 0.0f;
        outEnd = 0.0f;
        return false;
    }

    if (halfExtent.x > 0.0f && halfExtent.y > 0.0f)
    {
        float2 localOrigin = FBZZ_UnderwaterWorldToLocalXZ(origin.xz, center, cosYaw, sinYaw);
        float2 localDirection = float2(cosYaw * direction.x - sinYaw * direction.z,
                                       sinYaw * direction.x + cosYaw * direction.z);
        if (!FBZZ_UnderwaterClipAxis(localOrigin.x, localDirection.x,
                -halfExtent.x, halfExtent.x, intervalStart, intervalEnd)
            || !FBZZ_UnderwaterClipAxis(localOrigin.y, localDirection.y,
                -halfExtent.y, halfExtent.y, intervalStart, intervalEnd))
        {
            outStart = 0.0f;
            outEnd = 0.0f;
            return false;
        }
    }

    outStart = max(intervalStart, 0.0f);
    outEnd = min(intervalEnd, maxDistance);
    return outEnd - outStart > 1.0e-4f;
}

/// @brief Evaluates a zero-mean large-wave focus approximation for underwater direct light.
/// @param worldXZ Sample position in world XZ.
/// @param depth Distance below the mean water surface in meters.
/// @param waveAmplitude Aggregate wave height in meters.
/// @param waveFrequency Aggregate wave frequency in radians per meter.
/// @param waveSpeed Aggregate phase speed in radians per second.
/// @param time Simulation time in seconds.
/// @return Bounded multiplier whose contrast fades with water depth.
float FBZZ_UnderwaterWaveFocus(float2 worldXZ, float depth, float waveAmplitude,
                               float waveFrequency, float waveSpeed, float time)
{
    float frequency = max(waveFrequency, 1.0e-4f);
    float phaseA = dot(worldXZ, float2(1.0f, 0.37f)) * frequency + time * waveSpeed;
    float phaseB = dot(worldXZ, float2(-0.29f, 1.0f)) * (frequency * 1.37f) - time * (waveSpeed * 0.61f);
    float contrast = saturate(max(waveAmplitude, 0.0f) * 0.35f)
                   * exp(-max(depth, 0.0f) * frequency * 0.08f);
    return clamp(1.0f + contrast * sin(phaseA) * sin(phaseB), 0.5f, 1.5f);
}

#endif
