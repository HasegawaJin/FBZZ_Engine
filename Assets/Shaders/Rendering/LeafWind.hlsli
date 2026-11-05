/// @file    LeafWind.hlsli
/// @brief   Material-scoped leaf motion shared by color, shadow, selection and velocity.
/// @author  Hasegawa Jin
/// @date    2026-11-05
#ifndef FBZZ_LEAF_WIND_HLSLI
#define FBZZ_LEAF_WIND_HLSLI
/// @note Strength is in model metres; the root mask fixes the base and world phase decorrelates neighbouring trees.
float3 FbzzLeafWind(float3 localPosition, float2 uv, float4x4 objectWorld, float sampleTime)
{
    if (leafWindStrength <= 0.0f) return localPosition;
    const float3 origin = mul(float4(0, 0, 0, 1), objectWorld).xyz;
    const float phase = dot(origin.xz, float2(0.173f, 0.291f));
    const float mask = saturate((localPosition.y - leafWindRootHeight) * 0.5f);
    const float t = sampleTime * max(leafWindFrequency, 0.01f);
    const float gust = sin(t + phase) * 0.65f + sin(t * 0.47f + phase * 1.7f) * 0.35f;
    const float flutter = sin(t * 3.7f + dot(localPosition, float3(1.9f, 0.8f, 2.3f))) * (0.2f + 0.3f * saturate(uv.y));
    localPosition.xz += leafWindStrength * mask * float2(gust + flutter, gust * 0.36f - flutter * 0.4f);
    return localPosition;
}
#endif
