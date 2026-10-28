/// @file    DissolveNoise.hlsli
/// @brief   通常材質とスキン材質で共有する Dissolve 固有のノイズ。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#ifndef FBZZ_DISSOLVE_NOISE_HLSLI
#define FBZZ_DISSOLVE_NOISE_HLSLI

float DissolveHash21(float2 p)
{
    p = frac(p * float2(127.1f, 311.7f));
    p += dot(p, p + 19.19f);
    return frac(p.x * p.y);
}

float DissolveValueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0f - 2.0f * f);
    return lerp(lerp(DissolveHash21(i),                DissolveHash21(i + float2(1.0f, 0.0f)), u.x),
                lerp(DissolveHash21(i + float2(0.0f, 1.0f)), DissolveHash21(i + float2(1.0f, 1.0f)), u.x), u.y);
}

float DissolveMask(float2 uv)
{
    float n  = DissolveValueNoise(uv * 4.0f)         * 0.500f;
          n += DissolveValueNoise(uv * 8.0f)         * 0.250f;
          n += DissolveValueNoise(uv * 16.0f)        * 0.125f;
          n += DissolveValueNoise(uv * 32.0f)        * 0.125f;
    return saturate(n);
}

#endif ///< @note FBZZ_DISSOLVE_NOISE_HLSLI
