/// @file    ErrorMaterial.hlsl
/// @brief   ビルド時に埋め込む、通常・スキニング描画用のマゼンタ診断シェーダー。
/// @author  Hasegawa Jin
/// @date    2026-10-05

/// @note Camera b0 の viewProjection は 128 byte、Object b1 の world は先頭に格納する。
/// @see Docs/design/render-fallbacks.md
#include "Common/Binding.hlsli"
cbuffer CameraConstants : register(CB_CAMERA)
{
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
};
cbuffer ObjectConstants : register(CB_OBJECT)
{
    float4x4 world;
};
#if FBZZ_ERROR_SKINNED
cbuffer SkinningConstants : register(CB_SKINNING)
{
    float4x4 boneMatrices[128];
};
#endif

struct VertexInput
{
    float3 position : POSITION;
#if FBZZ_ERROR_SKINNED
    uint4 boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
#endif
};

float4 VSMain(VertexInput input) : SV_POSITION
{
    float4 position = float4(input.position, 1);
#if FBZZ_ERROR_SKINNED
    float4x4 skin = boneMatrices[input.boneIndices.x] * input.boneWeights.x
                 + boneMatrices[input.boneIndices.y] * input.boneWeights.y
                 + boneMatrices[input.boneIndices.z] * input.boneWeights.z
                 + boneMatrices[input.boneIndices.w] * input.boneWeights.w;
    position = mul(position, skin);
#endif
    return mul(mul(position, world), viewProjection);
}

float4 PSMain() : SV_Target0
{
    return float4(1, 0, 1, 1);
}
