/// @file    WaterLocalLights.hlsli
/// @brief   Shared Point/Spot inputs provide shadowed, attenuated sources for water-volume scattering.
/// @author  Hasegawa Jin
/// @date    2026-10-09
#ifndef FBZZ_WATER_LOCAL_LIGHTS_HLSLI
#define FBZZ_WATER_LOCAL_LIGHTS_HLSLI

#define FBZZ_PUNCTUAL_NO_DEBUG
#include "Rendering/Lighting.hlsli"
#include "Rendering/WaterMedium.hlsli"
static const uint WATER_LOCAL_LIGHT_LIMIT = 8u;

/// @brief Validates a shadow SRV before a physical water-light source reads it.
/// @return Missing geometry visibility is unavailable; the caller suppresses only sources that require that shadow map.
bool FBZZ_WaterShadowResourceAvailable(Texture2D<float> shadowMap, uint bindingSlot)
{
#if BINDLESS_SUPPORTED
    if (!IsBindlessValid(FbzzPixelSlot(bindingSlot))) return false;
#endif
    uint width;
    uint height;
    shadowMap.GetDimensions(width, height);
    return width > 0u && height > 0u;
}

/// @brief Evaluates raw punctual geometry visibility without a surface normal or decorative strength.
/// @param light Existing Point/Spot light record.
/// @param worldPosition Water-volume sample position.
/// @return Tile-clamped visibility; unassigned/out-of-frustum shadows are lit, assigned missing resources are dark.
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-samplecmplevelzero Comparison filtering.
float FBZZ_WaterLocalGeometryVisibility(PunctualLight light, float3 worldPosition)
{
    int slot = light.shadowIndex;
    if (slot < 0) return 1.0f;
    if (punctualShadowCount <= 0 || !FBZZ_WaterShadowResourceAvailable(gPunctualShadowAtlas, 28u)) return 0.0f;
    if (light.type == FBZZ_LIGHT_TYPE_POINT)
        slot += FBZZ_CubeFaceIndex(worldPosition - light.position);
    if (slot < 0 || slot >= punctualShadowCount) return 0.0f;
    float2 uv;
    float depth;
    WorldToShadowUV(worldPosition, punctualShadowVP[slot], uv, depth);
    if (any(uv < 0.0f) || any(uv > 1.0f) || depth < 0.0f || depth > 1.0f)
        return 1.0f;
    float4 tile = punctualShadowRect[slot];
    float2 texel = max(punctualShadowTexel, float2(1.0e-6f, 1.0e-6f));
    float2 inset = min(texel, tile.zw * 0.5f);
    float2 minimum = tile.xy + inset;
    float2 maximum = tile.xy + tile.zw - inset;
    float2 atlasUv = uv * tile.zw + tile.xy;
    float visibility = 0.0f;
    [unroll]
    for (int y = 0; y < 2; ++y)
        [unroll]
        for (int x = 0; x < 2; ++x) {
            float2 sampleUv = clamp(atlasUv + (float2(x, y) - 0.5f) * texel, minimum, maximum);
            visibility += gPunctualShadowAtlas.SampleCmpLevelZero(gPunctualShadowSamp,
                sampleUv, depth - punctualShadowParams[slot].x);
        }
    return visibility * 0.25f;
}

/// @brief Returns incoming Point/Spot radiance times phase and finite light-to-sample RGB Beer.
/// @param worldPosition Water-volume sample position.
/// @param fullPixelCenter Full-resolution pixel center, including its half-pixel offset.
/// @param V Normalized direction from sample to camera.
/// @param context Validated table/atlas dimensions retained by the volume operation.
/// @param medium Current sample returned by the caller's single point query.
/// @return Scattering source; the caller applies its analytic view-cell scattering weight once.
/// @note Cluster lookup uses each sample's view Z. Local sources remain active independently of Sun and sky intensity.
/// @see https://pbr-book.org/4ed/Volume_Scattering/Phase_Functions HG phase convention.
float3 FBZZ_EvaluateWaterLocalSource(PunctualLight light, float3 worldPosition,
    float3 V, FBZZWaterMediumQueryContext context, FBZZWaterMediumSample medium)
{
    if (light.type != FBZZ_LIGHT_TYPE_POINT && light.type != FBZZ_LIGHT_TYPE_SPOT) return 0.0f;
    if (light.intensity <= 0.0f || light.range <= 0.0f) return 0.0f;
    const float3 toLight = light.position - worldPosition;
    const float sourceDistance = length(toLight);
    if (sourceDistance >= light.range) return 0.0f;
    const float3 L = sourceDistance > 1.0e-6f ? toLight / sourceDistance : -V;
    float intensity = light.intensity * LightAttenuation(sourceDistance, light.range);
    if (light.type == FBZZ_LIGHT_TYPE_SPOT)
        intensity *= SpotConeWeight(L, light.direction, light.innerCos, light.outerCos);
    if (intensity <= 0.0f) return 0.0f;
    const float visibility = FBZZ_WaterLocalGeometryVisibility(light, worldPosition);
    if (visibility <= 0.0f) return 0.0f;
    float3 color = light.color;
    if (light.type == FBZZ_LIGHT_TYPE_SPOT && light.cookieIndex >= 0)
        color *= FBZZ_SampleLightCookie(light.cookieIndex, worldPosition);
    const float3 sourceBeer = FBZZ_WaterSourceRGBTransmittance(worldPosition, L, sourceDistance, context, medium);
    const float phase = WaterHenyeyGreensteinPhase(dot(-L, V), WaterOpticalClampPhaseG(medium.phaseG));
    return max(color, 0.0f) * (intensity * LIGHT_UNIT_SCALE * visibility * phase) * sourceBeer;
}

float3 FBZZ_WaterLocalLightSource(float3 worldPosition, float2 fullPixelCenter,
                                 float3 V, FBZZWaterMediumQueryContext context, FBZZWaterMediumSample medium)
{
    uint count;
    uint base;
    FBZZ_PunctualRange(fullPixelCenter, worldPosition, count, base);
    if (clusterLightMode == FBZZ_LIGHT_MODE_SAMPLED) {
        float3 estimate = 0.0f;
        /// @note Volume cells already integrate many spatial samples; keep their light budget independent of surface BRDF quality.
        /// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/megalights-in-unreal-engine Separate volumetric lighting sampling.
        const uint samples = min(FBZZ_PunctualEvaluationCount(count), 2u);
        /// @note Independent depth strata prevent repeatedly selecting the same two lights through a whole ray slice.
        const uint sequence = (uint)floor(max(mul(float4(worldPosition, 1.0f), view).z, 0.0f) * 8.0f);
        [loop] for (uint sampleIndex = 0u; sampleIndex < samples; ++sampleIndex) {
            const PunctualLight light = FBZZ_PunctualEvaluationAtBudget(base, count, sampleIndex, fullPixelCenter, worldPosition, samples, sequence);
            estimate += FBZZ_EvaluateWaterLocalSource(light, worldPosition, V, context, medium);
        }
        return estimate;
    }
    uint selected[8];
    float scores[8];
    [unroll]
    for (uint slot = 0; slot < WATER_LOCAL_LIGHT_LIMIT; ++slot) {
        selected[slot] = 0u;
        scores[slot] = -1.0f;
    }
    uint selectedCount = 0u;
    /// @note Retain eight strongest valid point/spot candidates before shadow and medium texture reads; the existing cluster/list candidate budget stays unchanged.
    [loop]
    for (uint candidate = 0; candidate < count; ++candidate) {
        PunctualLight light = FBZZ_PunctualAt(base, candidate);
        if (light.type != FBZZ_LIGHT_TYPE_POINT && light.type != FBZZ_LIGHT_TYPE_SPOT) continue;
        if (light.intensity <= 0.0f || light.range <= 0.0f) continue;
        float3 delta = light.position - worldPosition;
        float distance = length(delta);
        if (distance >= light.range) continue;
        float3 L = distance > 1.0e-6f ? delta / distance : -V;
        float intensity = light.intensity * LightAttenuation(distance, light.range);
        if (light.type == FBZZ_LIGHT_TYPE_SPOT)
            intensity *= SpotConeWeight(L, light.direction, light.innerCos, light.outerCos);
        float score = intensity * dot(max(light.color, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
        if (score <= 0.0f) continue;
        [unroll]
        for (uint rank = 0; rank < WATER_LOCAL_LIGHT_LIMIT; ++rank) {
            if (score <= scores[rank]) continue;
            for (int move = int(WATER_LOCAL_LIGHT_LIMIT) - 1; move > int(rank); --move) {
                selected[move] = selected[move - 1];
                scores[move] = scores[move - 1];
            }
            selected[rank] = candidate;
            scores[rank] = score;
            selectedCount = min(selectedCount + 1u, WATER_LOCAL_LIGHT_LIMIT);
            break;
        }
    }
    float3 source = 0.0f;
    [loop]
    for (uint index = 0; index < selectedCount; ++index) {
        PunctualLight light = FBZZ_PunctualAt(base, selected[index]);
        source += FBZZ_EvaluateWaterLocalSource(light, worldPosition, V, context, medium);
    }
    return source;
}

/// @brief Compatibility entry that resolves its receiver once and forwards to the cached light loop.
/// @pre ownerIndex identifies the caller's current optical medium.
float3 FBZZ_WaterLocalLightSource(float3 worldPosition, float2 fullPixelCenter,
                                 float3 V, uint ownerIndex, float phaseG)
{
    FBZZWaterMediumQueryContext context;
    FBZZWaterMediumSample medium;
    if (!FBZZ_WaterMediumHeader(context)
        || !FBZZ_WaterMediumBodySample(ownerIndex, worldPosition, context, true, false, medium)) return 0.0f;
    medium.phaseG = phaseG;
    return FBZZ_WaterLocalLightSource(worldPosition, fullPixelCenter, V, context, medium);
}
#endif
