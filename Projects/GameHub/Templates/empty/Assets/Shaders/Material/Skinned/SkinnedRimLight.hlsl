// FBZZ Engine
// Material/Skinned/SkinnedRimLight.hlsl | Material
// GPU スキニング + Blinn-Phong + リムライト + 法線マップ + PCF シャドウ
// PS ロジックは Surface/RimLight.hlsl と完全に一致させること。

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;        // RGBA ベースカラー                 offset 0
    float  roughness;     // スペキュラの広がり [0,1]          offset 16
    float  rimPower;      // リム輪郭の鋭さ [1,16]             offset 20
    float  rimIntensity;  // リム輝度スケール                  offset 24
    uint   textureMask;   // テクスチャ有効フラグ              offset 28
    float3 rimColor;      // リムライトの色                    offset 32
    float  _pad;          //                                  offset 44
};

Texture2D<float>       texShadow   : register(TEX_SHADOW);
Texture2D              texAlbedo   : register(TEX_ALBEDO);
Texture2D              texNormal   : register(TEX_NORMAL);
SamplerState           sampDefault : register(SAMPLER_DEFAULT);
SamplerComparisonState sampShadow  : register(SAMPLER_SHADOW);

float4x4 BlendSkinMatrix(SkinnedVSInput v)
{
    return boneMatrices[v.boneIndices.x] * v.boneWeights.x
         + boneMatrices[v.boneIndices.y] * v.boneWeights.y
         + boneMatrices[v.boneIndices.z] * v.boneWeights.z
         + boneMatrices[v.boneIndices.w] * v.boneWeights.w;
}

PSInput VSMain(SkinnedVSInput v)
{
    PSInput o;
    float4x4 skin    = BlendSkinMatrix(v);
    float4 localPos  = mul(float4(v.position, 1.0f), skin);
    float3 localN    = normalize(mul(v.normal,  (float3x3)skin));
    float3 localT    = normalize(mul(v.tangent, (float3x3)skin));
    float4 worldPos4 = mul(localPos, world);
    o.worldPos   = worldPos4.xyz;
    o.svPosition = mul(worldPos4, viewProjection);
    o.normal     = normalize(mul(localN, (float3x3)worldInvTranspose));
    o.tangent    = normalize(mul(localT, (float3x3)world));
    o.uv         = v.uv;
    return o;
}

float4 PSMain(PSInput p) : SV_Target0
{
    float3 col = (textureMask & 1u)
        ? texAlbedo.Sample(sampDefault, p.uv).rgb
        : albedo.rgb;

    float3 N = normalize(p.normal);
    if (textureMask & (1u << 1))
    {
        float3 normalSample = texNormal.Sample(sampDefault, p.uv).rgb;
        N = ApplyNormalMap(normalSample, N, normalize(p.tangent));
    }

    float3 V      = normalize(cameraPos - p.worldPos);
    float3 L      = normalize(-lightDir);
    float  shadow = ComputeShadow(texShadow, sampShadow, p.worldPos,
                                  lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    float3 result = Lighting_BlinnPhong(N, V, L, col, roughness,
                                        lightColor, lightIntensity, shadow);

    // 点光源 / スポットライト — 走査元は clusterLightMode が決める
    // (b3 の固定長配列 / StructuredBuffer / クラスタリスト)。
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        result += Lighting_BlinnPhong_Direct(N, V, ps.L, col, roughness,
            ps.color, ps.intensity);
    FBZZ_PUNCTUAL_END

    // リムライト: 法線と視線が直交する輪郭ほど明るくするフレネル近似。
    float NdotV = saturate(dot(N, V));
    float rim   = pow(1.0f - NdotV, max(rimPower, 1.0f));
    result += rimColor * rimIntensity * rim;

    return float4(result, 1.0f);
}
