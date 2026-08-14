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
    [branch]
    if (abs(temperature) > 1.0e-4f || abs(tint) > 1.0e-4f)
        color = ApplyWhiteBalance(color, temperature, tint);
    [branch]
    if (abs(hueDegrees) > 1.0e-4f)
        color = ApplyHueShift(color, hueDegrees);

    [branch]
    if (abs(contrastValue) > 1.0e-4f)
    {
        const float midpoint = 0.5f;
        color = (color - midpoint) * (1.0f + contrastValue) + midpoint;
    }

    [branch]
    if (abs(saturationValue - 1.0f) > 1.0e-4f)
    {
        const float luma = Luminance(color);
        color = lerp(float3(luma, luma, luma), color, saturationValue);
    }
    return saturate(color);
}

float3 ApplyVignette(float3 color,
                     float2 uv,
                     float intensity,
                     float smoothness,
                     float roundness,
                     float3 vignetteCol)
{
    if (intensity <= 0.0f)
        return color;

    float2 centered = uv * 2.0f - 1.0f;
    centered.x *= lerp(1.0f, screenSize.x / max(screenSize.y, 1.0f), saturate(roundness));
    float d = dot(centered, centered);
    float edge = smoothstep(1.0f - smoothness, 1.0f, d);
    return lerp(color, vignetteCol, edge * saturate(intensity));
}

float3 ApplyFilmGrain(float3 color, float2 uv, float intensity, float response)
{
    if (intensity <= 0.0f)
        return color;

    float noise = Hash2D(uv * screenSize + time * 97.0f) * 2.0f - 1.0f;
    float luma = Luminance(color);
    float weight = lerp(1.0f, 1.0f - saturate(luma), saturate(response));
    return saturate(color + noise * intensity * weight);
}

float2 ApplyPixelateUV(float2 uv, float pixelBlockSize)
{
    if (pixelBlockSize <= 1.0f)
        return uv;

    // WHAT: 画面座標を指定ピクセル単位のグリッド中央へ丸める。
    // WHY: 解像度依存の見た目を避けるため、UV ではなく screenSize 基準で量子化する。
    float2 block = max(float2(pixelBlockSize, pixelBlockSize), float2(1.0f, 1.0f));
    float2 pixel = floor(uv * screenSize / block) * block + block * 0.5f;
    return saturate(pixel / max(screenSize, float2(1.0f, 1.0f)));
}

float3 ApplySepia(float3 color, float intensity)
{
    if (intensity <= 0.0f)
        return color;

    float3 sepia = float3(
        dot(color, float3(0.393f, 0.769f, 0.189f)),
        dot(color, float3(0.349f, 0.686f, 0.168f)),
        dot(color, float3(0.272f, 0.534f, 0.131f)));
    return lerp(color, saturate(sepia), saturate(intensity));
}

float3 ApplyInvert(float3 color, float intensity)
{
    if (intensity <= 0.0f)
        return color;

    return lerp(color, float3(1.0f, 1.0f, 1.0f) - color, saturate(intensity));
}

float3 ApplyPosterize(float3 color, float levels)
{
    if (levels <= 1.0f)
        return color;

    float quantizedLevels = max(floor(levels), 2.0f);
    return floor(saturate(color) * quantizedLevels) / quantizedLevels;
}

float3 ApplyShadowHighlight(float3 color, float shadowAmount, float highlightAmount)
{
    if (shadowAmount <= 0.0f && highlightAmount <= 0.0f)
        return color;

    // WHAT: 暗部は持ち上げ、明部は軽く圧縮して白飛びを抑える。
    // WHY: HDR トーンマップ後の LDR に対する軽量な見た目補正として、露出を変えずに階調を残す。
    float luma = Luminance(color);
    float shadowMask = 1.0f - smoothstep(0.08f, 0.45f, luma);
    float highlightMask = smoothstep(0.55f, 1.0f, luma);
    color = lerp(color, color + (1.0f - color) * shadowAmount, shadowMask);
    color = lerp(color, color / (1.0f + color * highlightAmount * 2.0f), highlightMask);
    return saturate(color);
}

float3 ApplyColorFilter(float3 color, float3 filterColor, float intensity)
{
    if (intensity <= 0.0f)
        return color;

    // WHAT: ホワイトバランス後の最終色に薄いフィルター色を乗算する。
    // WHY: LUT を導入せず、昼/夕方/室内などのルックをシーン設定だけで寄せられるようにする。
    return saturate(lerp(color, color * max(filterColor, 0.0f), saturate(intensity)));
}

#endif // POSTPROCESS_HLSLI
