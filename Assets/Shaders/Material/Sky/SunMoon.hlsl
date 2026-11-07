/// @file    SunMoon.hlsl
/// @brief   スカイドームへ太陽・月・恒星の放射輝度を加える。
/// @author  Hasegawa Jin
/// @date    2026-07-01
#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Common/Random.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Atmosphere.hlsli"

struct SkyVSInput
{
    float3 position : POSITION;
};

struct SkyPSInput
{
    float4 svPosition : SV_POSITION;
    float3 rayDir     : TEXCOORD0;
};

SkyPSInput VSMain(SkyVSInput v)
{
    SkyPSInput o;
    o.svPosition = SkyClipPosition(v.position, view, projection);
    o.rayDir = v.position;
    return o;
}

/// @note 視線の立方体面に恒星を固定する。セル境界から離して配置し、解像度が足りない標本はフェードする。
float3 StarField(float3 ray, float pixelAngle)
{
    const uint cellCount = 112u;
    float3 axis = abs(ray);
    float2 plane;
    float major;
    uint face;
    if (axis.x >= axis.y && axis.x >= axis.z)
    {
        plane = ray.zy;
        major = axis.x;
        face = ray.x >= 0.0f ? 0u : 1u;
    }
    else if (axis.y >= axis.z)
    {
        plane = ray.xz;
        major = axis.y;
        face = ray.y >= 0.0f ? 2u : 3u;
    }
    else
    {
        plane = ray.xy;
        major = axis.z;
        face = ray.z >= 0.0f ? 4u : 5u;
    }
    float2 grid = clamp((plane / major * 0.5f + 0.5f) * cellCount, 0.0f, cellCount - 0.0001f);
    uint2 cell = uint2(grid);
    uint seed = cell.x + cellCount * (cell.y + cellCount * face);
    if (HashToFloat(seed) > 0.125f) return 0.0f;

    float2 center = lerp(0.21f, 0.79f, float2(HashToFloat(seed + 117007u), HashToFloat(seed + 230113u)));
    float2 delta = frac(grid) - center;
    float grade = pow(HashToFloat(seed + 430007u), 7.0f);
    float radius = lerp(0.027f, 0.070f, grade);
    float footprint = pixelAngle * (0.5f * cellCount) / major;
    float width = max(radius, footprint * 0.45f);
    /// @note 画素幅まで広げたときも放射束を保ち、星の明滅を抑える。
    float core = exp2(-2.4f * dot(delta, delta) / (width * width));
    float energy = core * (radius * radius) / (width * width);
    float visibility = 1.0f - smoothstep(0.24f, 0.53f, footprint);
    float3 tint = lerp(float3(0.68f, 0.82f, 1.0f), float3(1.0f, 0.84f, 0.65f),
        HashToFloat(seed + 760213u));
    return tint * lerp(0.42f, 3.0f, grade) * energy * visibility;
}

float LunarCraterRelief(float2 surface, float frequency, float footprint)
{
    float2 grid = surface * frequency;
    float2 cell = floor(grid);
    float2 offset = frac(grid) - lerp(0.25f, 0.75f, Hash2D2(cell + frequency));
    float radius = lerp(0.09f, 0.19f, Hash2D(cell + 18.0f));
    float distance = length(offset);
    float rim = 1.0f - smoothstep(0.0f, 0.21f, abs(distance / radius - 1.0f));
    float interior = 1.0f - smoothstep(radius * 0.52f, radius, distance);
    float lightSide = dot(offset, float2(-0.68f, 0.73f)) / max(distance, 1.0e-4f);
    return (rim * (0.05f + lightSide * 0.12f) - interior * 0.14f)
        * (1.0f - smoothstep(0.15f, 0.45f, footprint * frequency));
}

/// @note 玄武岩の海と明るい高地を手続き的に配置する。月の正面側を球面へ投影する。
/// @see https://science.nasa.gov/moon/moonlight/ Lunar nearside albedo.
float LunarAlbedo(float2 surface, float surfaceCosine, float footprint)
{
    float2 terrain = surface + (float2(ValueNoise2D(surface * 6.0f),
        ValueNoise2D(surface * 6.0f + 17.0f)) - 0.5f) * 0.16f;
    float mare = length((terrain - float2(-0.31f, 0.24f)) / float2(0.42f, 0.28f));
    mare = min(mare, length((terrain - float2(-0.23f, 0.46f)) / float2(0.30f, 0.22f)));
    mare = min(mare, length((terrain - float2(0.27f, 0.27f)) / float2(0.29f, 0.24f)));
    mare = min(mare, length((terrain - float2(0.51f, 0.01f)) / float2(0.17f, 0.19f)));
    mare = min(mare, length((terrain - float2(0.65f, 0.34f)) / float2(0.10f, 0.13f)));
    float highland = smoothstep(0.77f, 1.06f, mare);
    float ground = lerp(0.35f, 0.80f, highland);
    ground += (ValueNoise2D(terrain * 15.0f) - 0.5f) * lerp(0.11f, 0.24f, highland);
    ground += LunarCraterRelief(terrain, 5.0f, footprint) * highland;
    ground += LunarCraterRelief(terrain, 11.0f, footprint) * 0.60f * highland;
    float2 tycho = terrain - float2(-0.13f, -0.57f);
    float tychoDistance = length(tycho);
    float rayAngle = tychoDistance > 1.0e-5f ? atan2(tycho.y, tycho.x) : 0.0f;
    float rays = pow(saturate(0.55f + 0.45f * cos(rayAngle * 13.0f)), 5.0f);
    ground += 0.18f * rays * (1.0f - smoothstep(0.02f, 0.48f, tychoDistance));
    return max(ground, 0.0f) * lerp(0.66f, 1.0f, surfaceCosine);
}

float4 PSMain(SkyPSInput p) : SV_Target0
{
    float3 ray = normalize(p.rayDir);
    float3 sunDir = normalize(-lightDir);
    float horizon = HorizonMask(ray, cameraPos.y, planetRadius);
    float pixelAngle = max(length(ddx(ray)), length(ddy(ray)));

    float air = exp2(-max(ray.y, 0.0f) * 14.0f);
    float3 transmission = lerp(1.0f, float3(0.92f, 0.72f, 0.52f), air);
    float solarIntensity = max(sunIntensity * skyDimmer, 0.0f);
    float sunChord = length(ray - sunDir);
    float sunGlare = 0.020f * exp2(-sunChord * sunChord / 0.00016f)
        + 0.0025f * exp2(-sunChord * sunChord / 0.0016f);
    sunGlare *= smoothstep(-0.014f, 0.006f, sunDir.y);
    float3 color = (SunDisk(ray, sunDir, solarIntensity, cameraPos.y, planetRadius)
        + solarIntensity * sunGlare * horizon) * max(lightColor, 0.0f) * transmission;

    float3 moonDir = -sunDir;
    float moonChord = length(ray - moonDir);
    float moonRadius = radians(1.5f) * sqrt(max(moonSize, 0.05f));
    float moonCover = CelestialDiskCoverage(moonChord, moonRadius) * horizon;
    if (moonEnabled > 0.5f && moonBrightness > 0.0f)
    {
        float3 moonLight = max(moonColor, 0.0f) * moonBrightness * transmission;
        float halo = 0.023f * exp2(-moonChord * moonChord / 0.005f);
        color += moonLight * halo * horizon * smoothstep(-0.025f, 0.04f, moonDir.y);
        [branch] if (moonCover > 0.0f)
        {
            float3 referenceAxis = abs(moonDir.z) < 0.99f ? float3(0, 0, 1) : float3(0, 1, 0);
            float3 right = normalize(cross(moonDir, referenceAxis));
            float3 up = cross(moonDir, right);
            float2 surface = float2(dot(ray, right), dot(ray, up)) / moonRadius;
            float surfaceCosine = sqrt(saturate(1.0f - dot(surface, surface)));
            color += moonLight * LunarAlbedo(surface, surfaceCosine, pixelAngle / moonRadius) * moonCover;
        }
    }
    else moonCover = 0.0f;

    /// @see https://aa.usno.navy.mil/faq/RST_defs Civil and astronomical twilight (Sun at -6 and -18 degrees).
    float night = smoothstep(sin(radians(6.0f)), sin(radians(18.0f)), -sunDir.y);
    [branch] if (starBrightness > 0.0f && night > 0.0f && ray.y > 0.0f)
    {
        float haze = smoothstep(0.0f, 0.24f, ray.y);
        color += StarField(ray, pixelAngle) * starBrightness * night * haze * (1.0f - moonCover);
    }
    return float4(max(color, 0.0f), 1.0f);
}
