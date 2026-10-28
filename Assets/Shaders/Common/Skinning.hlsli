/// @file    Skinning.hlsli
/// @brief   4本のパレット行列を各描画パスで同じ順序に混合する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#ifndef FBZZ_SKINNING_HLSLI
#define FBZZ_SKINNING_HLSLI
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"
/// @note VS descriptors use the vertex index space, independently of PS/CS t14/t15.
FBZZ_VS_SBUFFER(float4x4, sharedBoneMatrices, VS_SB_BUFFER0_SLOT);
float4x4 ReadSkinMatrix(uint bone)
{
    if (objectParams.z > 0.5f) return sharedBoneMatrices[uint(objectParams.w) + bone];
    return boneMatrices[bone];
}

/// @note ウェイトは入力のまま使い、混合順序・パレット境界・行ベクトルの契約を変更しない。
float4x4 BlendSkinMatrix(SkinnedVSInput v)
{
    return ReadSkinMatrix(v.boneIndices.x) * v.boneWeights.x
         + ReadSkinMatrix(v.boneIndices.y) * v.boneWeights.y
         + ReadSkinMatrix(v.boneIndices.z) * v.boneWeights.z
         + ReadSkinMatrix(v.boneIndices.w) * v.boneWeights.w;
}

/// @note skin 後と world 後に xyz を取り w=1 を再構成する Mask 固有の変換順序を保つ。
float4 SkinningMaskClipPosition(SkinnedVSInput input, float4x4 worldMatrix, float4x4 viewProjectionMatrix)
{
    float4x4 skinMatrix = BlendSkinMatrix(input);
    float3 localPos = mul(float4(input.position, 1.0f), skinMatrix).xyz;
    float3 worldPos = mul(float4(localPos, 1.0f), worldMatrix).xyz;
    return mul(float4(worldPos, 1.0f), viewProjectionMatrix);
}

#endif ///< @note FBZZ_SKINNING_HLSLI
