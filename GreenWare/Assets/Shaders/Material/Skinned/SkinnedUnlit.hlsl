// FBZZ Engine
// Material/Skinned/SkinnedUnlit.hlsl | Material
// GPU スキニング + ライティングなし — アルベドをそのまま出力する
// PS ロジックは Surface/Unlit.hlsl と完全に一致させること。

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;
    uint   textureMask;
};

Texture2D    texAlbedo   : register(TEX_ALBEDO);
SamplerState sampDefault : register(SAMPLER_DEFAULT);

// WHY: Unlit はライティングを行わないため、PSInput の法線・接線・ワールド座標を
//      生成・補間しない。スキニング後の位置と UV だけをピクセル段へ渡す。
struct SkinnedUnlitPSInput
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

float4x4 BlendSkinMatrix(SkinnedVSInput v)
{
    return boneMatrices[v.boneIndices.x] * v.boneWeights.x
         + boneMatrices[v.boneIndices.y] * v.boneWeights.y
         + boneMatrices[v.boneIndices.z] * v.boneWeights.z
         + boneMatrices[v.boneIndices.w] * v.boneWeights.w;
}

SkinnedUnlitPSInput VSMain(SkinnedVSInput v)
{
    SkinnedUnlitPSInput o;
    float4x4 skin    = BlendSkinMatrix(v);
    float4 localPos  = mul(float4(v.position, 1.0f), skin);
    float4 worldPos4 = mul(localPos, world);
    o.svPosition = mul(worldPos4, viewProjection);
    o.uv         = v.uv;
    return o;
}

float4 PSMain(SkinnedUnlitPSInput p) : SV_Target0
{
    float3 color = (textureMask & 1u)
        ? texAlbedo.Sample(sampDefault, p.uv).rgb
        : albedo.rgb;
    return float4(color, 1.0f);
}
