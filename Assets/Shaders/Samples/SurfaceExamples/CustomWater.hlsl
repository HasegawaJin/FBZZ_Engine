/// @file    CustomWater.hlsl
/// @brief   Project-owned banded water pixel appearance over unchanged shared water geometry.
/// @author  Hasegawa Jin
/// @date    2026-10-24
/// @see Docs/design/water-appearance-authoring.md Shared vertex, view/depth and reflected material contracts.
#include "Water/WaterSurface.hlsli"
#include "Common/MaterialTextures.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float3 shallowTint;
    float depthBands;
    float3 deepTint;
    float depthRange;
    float3 foamTint;
    float foamWidth;
    float3 highlightTint;
    float highlightThreshold;
    float textureScale;
    float textureStrength;
    int lightBands;
    bool showFoam;
    uint textureMask;
    FBZZ_MATERIAL_TEX_ALBEDO
};

#if BINDLESS_SUPPORTED
FBZZ_MATERIAL_TEX(g_projectPattern, texAlbedoIndex);
#else
Texture2D g_projectPattern : register(t0);
#endif

float ProjectWaterBand(float value, float count)
{
    float levels = clamp(round(count), 2.0f, 12.0f);
    return floor(saturate(value) * (levels - 1.0f) + 0.5f) / (levels - 1.0f);
}

/// @note Only appearance is replaced: vertex displacement, patch morph, depth and exclusion inputs remain engine-owned.
float4 PSMain(WaterPSInput p) : SV_Target0
{
    if (WaterSurfaceExcluded(p)) discard;
    float2 screenUV = p.screenPos.xy / p.screenPos.w * float2(0.5f, -0.5f) + 0.5f;
    float rawDepth = g_sceneDepth.Sample(g_samplerClamp, screenUV).r;
    float viewDepth = max(mul(float4(p.worldPos, 1.0f), view).z, 0.0001f);
    float waterDepth = rawDepth > 0.0f ? max(LinearizeDepth(rawDepth) - viewDepth, 0.0f) : max(depthRange, 0.001f);
    if (p.splineSurface.w > 0.5f) waterDepth = min(waterDepth, max(p.splineSurface.x, 0.0f));
    float depthFactor = ProjectWaterBand(waterDepth / max(depthRange, 0.001f), depthBands);
    float3 normal = normalize(p.normal);
    float diffuse = ProjectWaterBand(saturate(dot(normal, normalize(-lightDir))), (float)lightBands);
    float3 illumination = max(ambientColor, 0.08f) + lightColor * max(lightIntensity, 0.0f) * diffuse;
    float3 color = lerp(shallowTint, deepTint, depthFactor) * illumination;
    if ((textureMask & 1u) != 0u) {
        float3 pattern = g_projectPattern.Sample(g_samplerTile, p.worldPos.xz * max(textureScale, 0.001f)).rgb;
        color *= lerp(1.0f, pattern, saturate(textureStrength));
    }
    float3 toEye = normalize(cameraPos - p.worldPos);
    float3 halfDirection = normalize(normalize(-lightDir) + toEye);
    float highlight = smoothstep(saturate(highlightThreshold) - 0.015f,
        saturate(highlightThreshold) + 0.015f, saturate(dot(normal, halfDirection)));
    color += highlightTint * highlight * max(lightIntensity, 0.0f);
    if (showFoam) {
        float shore = 1.0f - smoothstep(0.0f, max(foamWidth, 0.001f), waterDepth);
        float crest = step(0.45f, saturate(p.waveState.y));
        color = lerp(color, foamTint * illumination, max(shore, crest));
    }
    float footprint = max(length(ddx(p.worldPos.xz)), length(ddy(p.worldPos.xz)));
    float alpha = p.splineSurface.w > 0.5f ? 1.0f
        : WaterEdgeFade(p.uv, g_refractionFlowParams.zw, g_normalParams.xy, footprint);
    clip(alpha - 0.003f);
    if (waterWireframeMode > 0.5f) return float4(1.0f, 0.65f, 0.1f, 1.0f);
    return float4(max(color, 0.0f), alpha);
}
