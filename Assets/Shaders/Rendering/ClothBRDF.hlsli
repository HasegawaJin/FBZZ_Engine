/// @file    ClothBRDF.hlsli
/// @brief   Charlie sheen と Lambert 拡散による布の直接・環境照明。
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

/// @note radiance の mip 0 は未畳み込み環境。GGX の粗さ mip や DFG LUT は使わず、64 点の一様半球求積を行う。
/// @see https://google.github.io/filament/main/filament.html#lighting/imagebasedlights/cloth Charlie は専用の DG 積分を必要とする。
float3 ClothEnvironmentSheen(float3 normal, float3 view, float3 sheenColor, float roughness,
                              TextureCube radiance, SamplerState samplerState)
{
    if (max(sheenColor.r, max(sheenColor.g, sheenColor.b)) <= 0.0f) return 0.0f;
    const float3 up = abs(normal.z) < 0.999f ? float3(0,0,1) : float3(1,0,0);
    const float3 tangent = normalize(cross(up, normal));
    const float3 bitangent = cross(normal, tangent);
    float3 integral = 0.0f;
    [loop] for (uint i = 0; i < 64u; ++i) {
        const float noL = (float(i) + 0.5f) / 64.0f;
        const float phi = float(reversebits(i)) * 2.3283064365386963e-10f * 6.28318530718f;
        const float radius = sqrt(max(1.0f - noL * noL, 0.0f));
        float sine, cosine;
        sincos(phi, sine, cosine);
        const float3 light = tangent * (radius * cosine) + bitangent * (radius * sine) + normal * noL;
        integral += max(radiance.SampleLevel(samplerState, light, 0).rgb, 0.0f)
            * ClothDirect(normal, view, light, 0.0f, sheenColor, roughness);
    }
    return integral * (6.28318530718f / 64.0f);
}

#endif
