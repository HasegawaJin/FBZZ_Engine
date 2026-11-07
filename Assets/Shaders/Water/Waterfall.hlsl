/// @file    Waterfall.hlsl
/// @brief   Rigid waterfall curtains and impact foam over the standard mesh path.
/// @author  Hasegawa Jin
/// @date    2026-11-06
/// @see Docs/design/water-foundation.md Height-field water remains separate from visual curtains.
#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"

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
};

struct WaterfallPSInput
{
    float4 svPosition : SV_POSITION;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 normal : TEXCOORD2;
};

WaterfallPSInput VSMain(VSInput v)
{
    WaterfallPSInput o;
    const float4 position = mul(float4(v.position, 1.0f), world);
    o.svPosition = mul(position, viewProjection);
    o.uv = v.uv;
    o.worldPos = position.xyz;
    o.normal = normalize(mul(v.normal, (float3x3)worldInvTranspose));
    return o;
}

float WaterfallHash(float2 p)
{
    return frac(sin(dot(p, float2(127.1f, 311.7f))) * 43758.5453f);
}

float WaterfallNoise(float2 p)
{
    const float2 cell = floor(p);
    const float2 f = frac(p);
    const float2 w = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(WaterfallHash(cell), WaterfallHash(cell + float2(1, 0)), w.x),
        lerp(WaterfallHash(cell + float2(0, 1)), WaterfallHash(cell + 1.0f), w.x), w.y);
}

float4 PSMain(WaterfallPSInput p, bool frontFace : SV_IsFrontFace) : SV_Target0
{
    const float2 uv = p.uv;
    const float phase = time * flowSpeed;
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
