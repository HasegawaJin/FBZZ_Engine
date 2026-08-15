// FBZZ Engine
// Material/Surface/Toon.hlsl | Material
// セル/トゥーンシェーディング — NdotL を 3 段階に量子化して漫画風陰影を作る

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;       // RGBA ベースカラー  offset 0
    uint   textureMask;  // テクスチャフラグ   offset 16
};

Texture2D<float>       texShadow   : register(TEX_SHADOW);
Texture2D              texAlbedo   : register(TEX_ALBEDO);
SamplerState           sampDefault : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow  : register(SAMPLER_SHADOW);

PSInput VSMain(VSInput v)
{
    PSInput o;
    float4 worldPos4 = mul(float4(v.position, 1.0f), world);
    o.worldPos   = worldPos4.xyz;
    o.svPosition = mul(worldPos4, viewProjection);
    o.normal     = normalize(mul(v.normal,  (float3x3)worldInvTranspose));
    o.tangent    = normalize(mul(v.tangent, (float3x3)world));
    o.uv         = v.uv;
    return o;
}

float4 PSMain(PSInput p) : SV_Target0
{
    float3 col    = (textureMask & 1u)
        ? texAlbedo.Sample(sampDefault, p.uv).rgb
        : albedo.rgb;
    float3 N      = normalize(p.normal);
    float3 L      = normalize(-lightDir);
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    float3 result = Lighting_Toon(N, L, col, lightColor, lightIntensity, shadow);
    float3 V      = normalize(cameraPos - p.worldPos);

    // 点光源 / スポットライト — 走査元は clusterLightMode が決める
    // (b3 の固定長配列 / StructuredBuffer / クラスタリスト)。
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        result += Lighting_Toon_Direct(N, ps.L, col,
            ps.color, ps.intensity);
    FBZZ_PUNCTUAL_END

    // リムライト: 輪郭に明るいエッジを加えてセル感を強調
    float  rim = 1.0f - saturate(dot(N, V));
    rim = pow(rim, 3.0f);
    result += col * rim * 0.4f;

    return float4(result, 1.0f);
}
