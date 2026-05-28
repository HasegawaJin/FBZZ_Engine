// FBZZ Engine
// PostProcess.hlsli | Rendering
// Common full-screen post-processing operators
#ifndef POSTPROCESS_HLSLI
#define POSTPROCESS_HLSLI

#include "Common/Color.hlsli"
#include "Common/Random.hlsli"

float2 LensDistortUV(float2 uv, float amount)
{
    float2 centered = uv * 2.0f - 1.0f;
    float r2 = dot(centered, centered);
    float scale = 1.0f + amount * r2;
    return centered * scale * 0.5f + 0.5f;
}

float3 ApplyWhiteBalance(float3 color, float temperature, float tint)
{
    float3 balance = float3(
        1.0f + temperature * 0.08f - tint * 0.03f,
        1.0f + tint * 0.06f,
        1.0f - temperature * 0.08f - tint * 0.03f);
    return max(color * max(balance, 0.0f), 0.0f);
}

float3 ApplyHueShift(float3 color, float hueDegrees)
{
    float angle = radians(hueDegrees);
    float s = sin(angle);
    float c = cos(angle);
    float3x3 m = float3x3(
        0.299f + 0.701f * c + 0.168f * s, 0.587f - 0.587f * c + 0.330f * s, 0.114f - 0.114f * c - 0.497f * s,
        0.299f - 0.299f * c - 0.328f * s, 0.587f + 0.413f * c + 0.035f * s, 0.114f - 0.114f * c + 0.292f * s,
        0.299f - 0.300f * c + 1.250f * s, 0.587f - 0.588f * c - 1.050f * s, 0.114f + 0.886f * c - 0.203f * s);
    return saturate(mul(m, color));
}

float3 ApplyColorAdjustments(float3 color,
                             float contrastValue,
                             float saturationValue,
                             float hueDegrees,
                             float temperature,
                             float tint)
{
    color = ApplyWhiteBalance(color, temperature, tint);
    color = ApplyHueShift(color, hueDegrees);

    float midpoint = 0.5f;
    color = (color - midpoint) * (1.0f + contrastValue) + midpoint;

    float luma = Luminance(color);
    color = lerp(float3(luma, luma, luma), color, saturationValue);
    return saturate(color);
}

float3 ApplyVignette(float3 color,
                     float2 uv,
                     float intensity,
                     float smoothness,
                     float roundness,
                     float3 vignetteCol)
{
    float2 centered = uv * 2.0f - 1.0f;
    centered.x *= lerp(1.0f, screenSize.x / max(screenSize.y, 1.0f), saturate(roundness));
    float d = dot(centered, centered);
    float edge = smoothstep(1.0f - smoothness, 1.0f, d);
    return lerp(color, vignetteCol, edge * saturate(intensity));
}

float3 ApplyFilmGrain(float3 color, float2 uv, float intensity, float response)
{
    float noise = Hash2D(uv * screenSize + time * 97.0f) * 2.0f - 1.0f;
    float luma = Luminance(color);
    float weight = lerp(1.0f, 1.0f - saturate(luma), saturate(response));
    return saturate(color + noise * intensity * weight);
}

#endif // POSTPROCESS_HLSLI
