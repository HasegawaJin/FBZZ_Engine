/// @file    FallbackSkinned.hlsl
/// @brief   FallbackSkinned.hlsl の既存描画と共通頂点契約。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#ifndef FALLBACK_SKINNED_HLSL
#define FALLBACK_SKINNED_HLSL

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/MaterialVertex.hlsli"
#include "Common/Skinning.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/MaterialTextures.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 albedo;
    uint   textureMask;

    /// @note bindless のテクスチャ添字。Material::Upload が毎フレーム書き込む。
    /// @note ここに宣言した枠だけが Inspector に出る (Common/MaterialTextures.hlsli)。
    uint texAlbedoIndex;
};

FBZZ_MATERIAL_TEX(texAlbedo, texAlbedoIndex);
SamplerState sampDefault : register(SAMPLER_DEFAULT);



PSInput VSMain(SkinnedVSInput v)
{
    return TransformSkinnedMaterialVertex(v);
}

float4 PSMain(PSInput p) : SV_Target0
{
    float3 color = (textureMask & 1u)
        ? texAlbedo.Sample(sampDefault, p.uv).rgb
        : albedo.rgb;
    return float4(color, 1.0f);
}

#endif /// @note FALLBACK_SKINNED_HLSL