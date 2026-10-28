/// @file    PostProcess.hlsli
/// @brief   全画面の色調整と画面効果を共有する。
/// @author  Hasegawa Jin
/// @date    2026-05-27
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

/// @note 画面の縦横比で円を保ち、二乗した帯域で輪の縁の変位と傾きをゼロにする。
float2 ShockRingUV(float2 uv, float2 center, float radius, float width, float amplitude)
{
    if (amplitude <= 1.0e-5f || width <= 1.0e-5f)
        return uv;

    const float aspect = screenSize.x / max(screenSize.y, 1.0f);
    const float2 toPixel = float2((uv.x - center.x) * aspect, uv.y - center.y);
    const float radial = length(toPixel);
    if (radial <= 1.0e-5f)
        return uv;

    const float s = clamp((radial - radius) / width, -1.0f, 1.0f);
    const float band = 1.0f - s * s;
    const float2 outward = toPixel / radial;
    /// @note 変位量は画面空間、戻り値は UV 空間。
    return uv + float2(outward.x / aspect, outward.y) * (amplitude * band * band);
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
    /// @note 既定の設定では全画面の行列演算と三角関数を省く。
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

    /// @note ブロック幅は UV でなく画素単位。
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

    /// @note 露出を変えず、トーンマップ後の LDR の階調を補正する。
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

    return saturate(lerp(color, color * max(filterColor, 0.0f), saturate(intensity)));
}

/// @note Composite と HDR 履歴の再構成で同じ順序を使い、色調整・LUT・フェードの二重適用を防ぐ。
float3 ApplyFinalColorEffects(float3 ldr, float2 uv, Texture3D lutTexture, SamplerState lutSampler)
{
    ldr = ApplyColorAdjustments(ldr, contrast, saturation, hueShift, temperature, tint);
    ldr = ApplyShadowHighlight(ldr, shadowLift, highlightCompression);
    ldr = ApplyColorFilter(ldr, colorFilter, colorFilterIntensity);
    ldr = ApplySepia(ldr, sepiaIntensity);
    ldr = ApplyInvert(ldr, invertIntensity);
    ldr = ApplyPosterize(ldr, posterizeLevels);
    ldr = ApplyVignette(ldr, uv, vignetteIntensity, vignetteSmoothness, vignetteRoundness, vignetteColor);
    ldr = ApplyFilmGrain(ldr, uv, filmGrainIntensity, filmGrainResponse);
    if (lutBlend > 0.0f)
    {
        /// @note 32³ LUT の両端をテクセル中心へ合わせる。
        const float lutSize = 32.0f;
        const float3 lutUV = saturate(ldr) * ((lutSize - 1.0f) / lutSize) + (0.5f / lutSize);
        ldr = lerp(ldr, lutTexture.SampleLevel(lutSampler, lutUV, 0.0f).rgb, saturate(lutBlend));
    }
    /// @note 明るさはフェードより先に掛け、暗転の黒を持ち上げない。
    ldr *= max(userBrightness, 0.0f);
    if (screenFadeAlpha > 0.0f)
        ldr = lerp(ldr, screenFadeColor, saturate(screenFadeAlpha));
    return ldr;
}

#endif
