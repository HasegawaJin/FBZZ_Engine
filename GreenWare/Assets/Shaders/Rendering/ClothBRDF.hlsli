/// @file    ClothBRDF.hlsli
/// @brief   Charlie sheen と Lambert 拡散による布の直接照明。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#ifndef FBZZ_CLOTH_BRDF_HLSLI
#define FBZZ_CLOTH_BRDF_HLSLI

/// @see https://google.github.io/filament/main/filament.html#materialsystem/cloth/clothspecularbrdf Charlie NDF と Neubelt visibility。
float3 ClothDirect(float3 normal, float3 view, float3 light, float3 baseColor,
                   float3 sheenColor, float perceptualRoughness)
{
    const float noL = saturate(dot(normal, light));
    const float noV = max(saturate(dot(normal, view)), 0.0001f);
    const float3 halfVector = SafeNormalize(view + light, normal);
    const float noH = saturate(dot(normal, halfVector));
    const float alpha = max(perceptualRoughness * perceptualRoughness, 0.025f);
    const float exponent = rcp(alpha);
    const float distribution = (2.0f + exponent) * pow(max(1.0f - noH * noH, 0.0f), 0.5f * exponent)
        * 0.1591549431f;
    const float visibility = rcp(max(4.0f * (noL + noV - noL * noV), 0.0001f));
    return (baseColor * 0.3183098862f + saturate(sheenColor) * distribution * visibility) * noL;
}

#endif
