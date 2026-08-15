// FBZZ Engine
// SurfaceCommon.hlsli | Rendering
// カスタムサーフェスシェーダーが共有できるテクスチャ・法線・UV の標準処理
#ifndef SURFACE_COMMON_HLSLI
#define SURFACE_COMMON_HLSLI

#include "Common/Color.hlsli"
#include "Common/Space.hlsli"

// WHY: マテリアルのテクスチャ有無を各シェーダーが個別に解釈すると、
//      フォールバック色・sRGB変換・チャンネル規約が簡単にずれる。
//      ビット位置を共通 API に固定し、ユーザーシェーダーの自由な PS ロジックと
//      エンジン側の MaterialAsset の契約を分離する。
static const uint FBZZ_SURFACE_ALBEDO = 1u << 0;
static const uint FBZZ_SURFACE_NORMAL = 1u << 1;
static const uint FBZZ_SURFACE_METALLIC_ROUGHNESS = 1u << 2;
static const uint FBZZ_SURFACE_EMISSIVE = 1u << 3;
static const uint FBZZ_SURFACE_OCCLUSION = 1u << 4;

float3 SurfaceSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-10f
        ? value * rsqrt(lengthSquared)
        : fallback;
}

float2 SurfaceTransformUv(float2 uv, float2 tiling, float2 offset)
{
    return uv * tiling + offset;
}

float4 SurfaceSampleAlbedo(Texture2D textureMap, SamplerState samplerState,
                           float2 uv, float4 tint, uint textureMask)
{
    // textureMask はドロー単位で一定なので、branch hint により未使用テクスチャの
    // サンプルを確実にスキップしやすくする。これは全ピクセルで同じ分岐になる。
    float4 texel = float4(1.0f, 1.0f, 1.0f, 1.0f);
    [branch]
    if ((textureMask & FBZZ_SURFACE_ALBEDO) != 0u)
        texel = textureMap.Sample(samplerState, uv);

    return float4(SRGBToLinear(texel.rgb) * tint.rgb, texel.a * tint.a);
}

float3 SurfaceSampleNormal(Texture2D normalMap, SamplerState samplerState,
                           float2 uv, float3 geometricNormal, float3 tangent,
                           float normalStrength, uint textureMask)
{
    float3 normal = SurfaceSafeNormalize(geometricNormal, float3(0.0f, 1.0f, 0.0f));
    [branch]
    if ((textureMask & FBZZ_SURFACE_NORMAL) != 0u)
    {
        const float3 tangentNormal = ApplyNormalMap(
            normalMap.Sample(samplerState, uv).rgb, normal,
            SurfaceSafeNormalize(tangent, float3(1.0f, 0.0f, 0.0f)));
        normal = SurfaceSafeNormalize(
            lerp(normal, tangentNormal, saturate(normalStrength)), normal);
    }
    return normal;
}

// Metallic-Roughness は glTF 規約 (G=roughness, B=metallic) を統一する。
float2 SurfaceSampleMetallicRoughness(Texture2D textureMap, SamplerState samplerState,
                                      float2 uv, float2 fallback, uint textureMask)
{
    float2 result = fallback;
    [branch]
    if ((textureMask & FBZZ_SURFACE_METALLIC_ROUGHNESS) != 0u)
        result = textureMap.Sample(samplerState, uv).gb;
    return float2(saturate(result.x), saturate(result.y));
}

float SurfaceSampleOcclusion(Texture2D textureMap, SamplerState samplerState,
                             float2 uv, float strength, uint textureMask)
{
    float occlusion = 1.0f;
    [branch]
    if ((textureMask & FBZZ_SURFACE_OCCLUSION) != 0u)
        occlusion = textureMap.Sample(samplerState, uv).r;
    return lerp(1.0f, saturate(occlusion), saturate(strength));
}

float3 SurfaceSampleEmissive(Texture2D textureMap, SamplerState samplerState,
                             float2 uv, float3 color, float scale, uint textureMask)
{
    float3 texel = float3(1.0f, 1.0f, 1.0f);
    [branch]
    if ((textureMask & FBZZ_SURFACE_EMISSIVE) != 0u)
        texel = SRGBToLinear(textureMap.Sample(samplerState, uv).rgb);
    return texel * color * scale;
}

#endif // SURFACE_COMMON_HLSLI
