/// @file    Waterfall.hlsl
/// @brief   Waterfall surface, breakup, and downstream foam over the transparent mesh path.
/// @author  Hasegawa Jin
/// @date    2026-11-06
/// @see Docs/design/water-foundation.md Height-field water remains separate from visual curtains.
#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D_T(float, waterfallShadowMap, TEX_SHADOW_SLOT);
SamplerComparisonState waterfallShadowSampler : register(SAMPLER_SHADOW);

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;
    float4 foamColor;
    /// @note UV repeats per second; positive moves from UV v=0 at the lip toward v=1 at the foot.
    float flowSpeed;
    float streamDensity;
    float foamAmount;
    float opacity;
    /// @note Zero selects a curtain; one selects an XZ impact-foam mesh.
    float foamMode;
    float edgeSoftness;
    /// @note Generated vertices carry travel time [s], breakup, seam coverage, and surface role in COLOR.
    float geometryMode;
};

struct WaterfallPSInput
{
    float4 svPosition : SV_POSITION;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float4 fallData : TEXCOORD3;
    nointerpolation float phaseOffset : TEXCOORD4;
};

WaterfallPSInput VSMain(VSInputColor v)
{
    WaterfallPSInput o;
    float4 position = mul(float4(v.position, 1.0f), world);
    o.normal = normalize(mul(v.normal, (float3x3)worldInvTranspose));
    const float2 origin = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), world).xz;
    o.phaseOffset = frac(dot(origin, float2(0.173f, 0.269f))) * 2.35f;
    if (geometryMode > 0.5f && v.color.w < 0.5f) {
        const float t = saturate(v.uv.y);
        const float envelope = 4.0f * t * (1.0f - t);
        const float phase = (v.color.x - cloudShadowTime * flowSpeed + o.phaseOffset) * 12.0f;
        const float surge = sin(v.uv.x * 31.0f + phase) * sin(v.uv.x * 12.0f - phase * 0.39f);
        position.xyz += o.normal * (envelope * surge * min(v.color.y, 2.0f) * v.tangent.x);
    }
    o.svPosition = mul(position, viewProjection);
    o.uv = v.uv;
    o.worldPos = position.xyz;
    o.fallData = v.color;
    return o;
}

float WaterfallHash(float2 p)
{
    float3 h = frac(p.xyx * float3(0.1031f, 0.11369f, 0.13787f));
    h += dot(h, h.yzx + 19.19f);
    return frac((h.x + h.y) * h.z);
}

float WaterfallNoise(float2 p)
{
    const float2 cell = floor(p);
    const float2 f = frac(p);
    const float2 w = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(WaterfallHash(cell), WaterfallHash(cell + float2(1, 0)), w.x),
        lerp(WaterfallHash(cell + float2(0, 1)), WaterfallHash(cell + 1.0f), w.x), w.y);
}

float FilteredWaterfallNoise(float2 p)
{
    const float2 footprint = fwidth(p);
    const float visible = 1.0f - smoothstep(0.40f, 1.15f, max(footprint.x, footprint.y));
    [branch] if (visible <= 0.0f) return 0.5f;
    return lerp(0.5f, WaterfallNoise(p), visible);
}

float3 ShadeWaterfallWater(float3 worldPos, float3 surfaceNormal, float3 opticalNormal, float aeration)
{
    const float3 toView = cameraPos - worldPos;
    const float3 viewDir = toView * rsqrt(max(dot(toView, toView), 1e-5f));
    const float3 toSun = normalize(-lightDir + float3(0.0f, -0.0001f, 0.0f));
    const float strength = max(lightIntensity, 0.0f);
    float shadow = 1.0f;
    [branch] if (strength > 0.0f)
        shadow = ComputeShadow(waterfallShadowMap, waterfallShadowSampler, worldPos, lightViewProjection,
            shadowMapTexelSize, shadowBias, surfaceNormal, toSun);
    /// @note A bounded direct term preserves the contrast between clear water and aerated pockets in HDR.
    const float3 sun = lightColor * (strength / (1.0f + strength * 0.36f)) * shadow;
    /// @note The terrain shadow attenuates direct sun. Open-sky reflection and diffuse illumination remain in shade.
    const float3 ambient = max(ambientColor, float3(0.23f, 0.29f, 0.35f));
    const float facing = saturate(dot(opticalNormal, viewDir));
    const float fresnel = 0.02f + 0.98f * pow(1.0f - facing, 5.0f);
    const float3 halfVector = viewDir + toSun;
    const float3 halfDir = halfVector * rsqrt(max(dot(halfVector, halfVector), 1e-5f));
    const float highlight = pow(saturate(dot(opticalNormal, halfDir)), lerp(86.0f, 23.0f, aeration));
    const float cosine = saturate(dot(opticalNormal, toSun));
    const float backlight = pow(saturate(dot(-viewDir, toSun)), 5.0f);
    const float3 water = albedo.rgb * (ambient * 0.57f + sun * (0.10f + 0.24f * cosine));
    const float3 bubbles = foamColor.rgb * (ambient * 0.91f + sun * (0.10f + 0.34f * cosine + 0.09f * backlight));
    return lerp(water, bubbles, aeration)
        + (ambient * (0.13f + fresnel * 0.44f) + sun * highlight * 0.50f) * (1.0f - 0.67f * aeration);
}

float4 ShadeBakedLiquid(WaterfallPSInput p, bool frontFace)
{
    const float3 normal = frontFace ? normalize(p.normal) : -normalize(p.normal);
    const float turbulence = FilteredWaterfallNoise(p.worldPos.xz * 4.3f + p.worldPos.y * float2(1.7f, -2.9f));
    const float aeration = lerp(0.16f, 0.72f, smoothstep(0.27f, 0.70f, turbulence));
    const float3 color = ShadeWaterfallWater(p.worldPos, normal, normal, aeration);
    const float coverage = p.fallData.z * lerp(0.42f, 0.91f, aeration);
    return float4(color, saturate(coverage * opacity * albedo.a));
}

float4 ShadeGeneratedFall(WaterfallPSInput p, bool frontFace)
{
    const float2 uv = p.uv;
    const float seconds = cloudShadowTime - p.phaseOffset;
    const float3 normal = frontFace ? normalize(p.normal) : -normalize(p.normal);
    float aeration = 0.0f;
    float coverage = 0.0f;
    float3 opticalNormal = normal;
    if (p.fallData.w > 0.5f) {
        const float advected = uv.y - seconds * p.fallData.x / max(p.fallData.z, 0.25f);
        const float across = abs(uv.x * 2.0f - 1.0f);
        const float eddy = FilteredWaterfallNoise(float2(uv.x * 5.7f, advected * 3.4f));
        const float islands = FilteredWaterfallNoise(float2(uv.x * 16.0f + eddy * 1.7f, advected * 8.2f + eddy));
        const float fragments = smoothstep(0.39f, 0.76f, islands * 0.73f + eddy * 0.27f);
        const float edge = 1.0f - smoothstep(0.44f, 1.0f, across + (eddy - 0.5f) * uv.y * 0.28f);
        const float falloff = exp2(-uv.y * 3.1f) * (1.0f - smoothstep(0.50f, 1.0f, uv.y));
        coverage = saturate(edge * falloff * (0.18f + fragments * 0.76f) * p.fallData.y * 0.64f);
        aeration = lerp(0.55f, 0.95f, fragments);
    } else {
        const float phase = p.fallData.x - seconds * flowSpeed;
        const float breakup = saturate(p.fallData.y * 0.65f);
        const float travel = p.fallData.x / max(uv.y, 0.001f);
        const float impactBand = clamp(0.55f / max(4.905f * travel * travel, 0.01f), 0.001f, 0.045f);
        const float seam = max(1.0f - smoothstep(0.0f, 0.025f, uv.y), smoothstep(1.0f - impactBand, 1.0f, uv.y));
        const float pulse = FilteredWaterfallNoise(float2(uv.x * 4.2f, phase * 2.6f));
        const float crossFlow = (pulse - 0.5f) * (1.2f + breakup * 1.5f);
        const float2 lane = float2(uv.x * max(1.0f, streamDensity * 0.48f) + crossFlow,
            phase * (3.6f + breakup * 1.8f) + uv.x * 2.3f);
        const float stream = FilteredWaterfallNoise(lane);
        const float chute = smoothstep(0.59f, 0.97f, uv.y);
        /// @note A metre-based layer keeps the impact breakup visible on waterfalls with long travel times.
        const float2 detailUv = float2(lane.x * 2.2f + pulse,
            lerp(lane.y * 2.7f + 3.1f, p.worldPos.y * 0.82f + seconds * flowSpeed * 6.3f + lane.x * 0.31f, chute));
        const float detail = FilteredWaterfallNoise(detailUv);
        const float foamMask = smoothstep(0.31f, 0.73f, stream * 0.42f + pulse * 0.28f + detail * 0.30f);
        const float wash = chute * smoothstep(0.27f, 0.75f, detail) * 0.33f;
        aeration = saturate((0.11f + foamMask * lerp(0.62f, 0.91f, breakup) + wash) * foamAmount);
        const float outside = min(uv.x, 1.0f - uv.x);
        const float edge = smoothstep(0.0f, max(edgeSoftness, 0.025f) + 0.09f * breakup * (1.0f - pulse), outside);
        const float core = lerp(0.24f, 0.82f, saturate(0.45f * stream + aeration * 0.72f));
        coverage = p.fallData.z * lerp(edge * core, max(core, 0.49f), seam);
        /// @note Cross and down slopes modulate reflections while the river contact bands keep their base normals.
        const float3 across = cross(normal, float3(0.0f, 1.0f, 0.0f));
        const float3 down = cross(normal, across);
        opticalNormal = normalize(normal + (1.0f - seam)
            * (across * (stream - detail) * 0.82f + down * (detail - pulse) * 0.64f));
    }
    return float4(ShadeWaterfallWater(p.worldPos, normal, opticalNormal, aeration), saturate(coverage * opacity * albedo.a));
}

float4 PSMain(WaterfallPSInput p, bool frontFace : SV_IsFrontFace) : SV_Target0
{
    if (geometryMode > 0.5f) {
        if (p.fallData.w > 1.5f) return ShadeBakedLiquid(p, frontFace);
        return ShadeGeneratedFall(p, frontFace);
    }
    const float2 uv = p.uv;
    const float phase = cloudShadowTime * flowSpeed;
    const float density = max(streamDensity, 1.0f);
    const float breakup = WaterfallNoise(float2(uv.x * 6.0f + 3.0f, uv.y * 14.0f - phase * 2.0f));
    const float warpedU = uv.x + (breakup - 0.5f) * 0.012f;
    const float n0 = WaterfallNoise(float2(warpedU * density, uv.y * 12.0f - phase * 2.4f));
    const float n1 = WaterfallNoise(float2(warpedU * density * 2.7f + 7.0f, uv.y * 43.0f - phase * 5.1f));
    const float streak = saturate(n0 * 0.68f + n1 * 0.32f);
    const float edge = max(edgeSoftness, 0.001f);
    const float sideFade = smoothstep(0.0f, edge, uv.x) * smoothstep(0.0f, edge, 1.0f - uv.x);
    const float endFade = smoothstep(0.0f, 0.035f, uv.y) * smoothstep(0.0f, 0.07f, 1.0f - uv.y);
    const float curtainFoam = saturate((streak - 0.52f) * 2.2f + breakup * 0.16f + uv.y * uv.y * 0.28f) * foamAmount;
    float alpha = sideFade * endFade * opacity * lerp(0.2f, 0.85f, streak) * lerp(0.55f, 1.0f, breakup);
    float foam = curtainFoam;
    if (foamMode > 0.5f) {
        const float2 radial = (uv - 0.5f) * 2.0f;
        const float radius = length(radial);
        const float noise = WaterfallNoise(radial * 8.0f + float2(phase * 0.3f, phase * 0.17f));
        const float rings = 0.5f + 0.5f * sin(radius * 36.0f - phase * 6.0f + noise * 4.0f);
        alpha = (1.0f - smoothstep(0.5f, 1.0f, radius)) * opacity
            * saturate(noise * foamAmount + rings * 0.28f);
        foam = saturate(0.65f + noise * foamAmount);
    }
    const float3 normal = frontFace ? normalize(p.normal) : -normalize(p.normal);
    const float diffuse = 0.35f + 0.65f * saturate(dot(normal, normalize(-lightDir)));
    const float3 illumination = max(ambientColor, 0.035f) + lightColor * max(lightIntensity, 0.0f) * diffuse * 0.35f;
    float3 color = lerp(albedo.rgb, foamColor.rgb, saturate(foam)) * illumination;
    if (foamMode <= 0.5f) {
        const float3 toCamera = cameraPos - p.worldPos;
        const float3 viewDirection = toCamera * rsqrt(max(dot(toCamera, toCamera), 0.0001f));
        const float facing = saturate(dot(normal, viewDirection));
        const float rim = pow(1.0f - facing, 4.0f);
        const float3 halfVector = viewDirection + normalize(-lightDir);
        const float3 halfDirection = halfVector * rsqrt(max(dot(halfVector, halfVector), 0.0001f));
        const float glint = pow(saturate(dot(normal, halfDirection)), 48.0f) * lerp(0.3f, 1.0f, n1);
        /// @note Surface orientation preserves the curved stream silhouette independently of UV streaks.
        color *= lerp(0.82f, 1.08f, facing);
        color += lightColor * max(lightIntensity, 0.0f) * (glint * 0.28f + rim * 0.07f)
            * (1.0f - saturate(foam) * 0.6f);
        alpha *= lerp(0.82f, 1.0f, rim);
    }
    return float4(color, saturate(alpha * albedo.a));
}
