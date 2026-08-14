// FBZZ Engine
// Material/Surface/Unlit.hlsl | Material
// ライティングなし — アルベドをそのまま出力する
#ifndef UNLIT_HLSL
#define UNLIT_HLSL

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;       // RGBA ベースカラー  offset 0
    uint   textureMask;  // テクスチャフラグ   offset 16
};

Texture2D    texAlbedo   : register(TEX_ALBEDO);
SamplerState sampDefault : register(SAMPLER_DEFAULT);

// WHY: Unlit はライティングを行わないため、PSInput の法線・接線・ワールド座標を
//      生成・補間しない。帯域と頂点シェーダーの演算を最小化しつつ、UV は維持する。
struct UnlitPSInput
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

UnlitPSInput VSMain(VSInput v)
{
    UnlitPSInput o;
    float4 worldPos4 = mul(float4(v.position, 1.0f), world);
    o.svPosition = mul(worldPos4, viewProjection);
    o.uv         = v.uv;
    return o;
}

float4 PSMain(UnlitPSInput p) : SV_Target0
{
    float3 color = (textureMask & 1u)
        ? texAlbedo.Sample(sampDefault, p.uv).rgb
        : albedo.rgb;
    return float4(color, 1.0f);
}

#endif // UNLIT_HLSL
