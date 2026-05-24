// FBZZ Engine
// SkinnedShadowMap.hlsl | Pipeline
// Depth-only shadow map pass for GPU-skinned meshes

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Platform/DX11.hlsli"

struct SMPSInput
{
    float4 svPosition : SV_POSITION;
};

float4x4 BlendSkinMatrix(SkinnedVSInput v)
{
    return boneMatrices[v.boneIndices.x] * v.boneWeights.x
         + boneMatrices[v.boneIndices.y] * v.boneWeights.y
         + boneMatrices[v.boneIndices.z] * v.boneWeights.z
         + boneMatrices[v.boneIndices.w] * v.boneWeights.w;
}

SMPSInput VSMain(SkinnedVSInput v)
{
    SMPSInput o;
    float4 localPos = mul(float4(v.position, 1.0f), BlendSkinMatrix(v));
    float4 worldPos = mul(localPos, world);
    o.svPosition = mul(worldPos, viewProjection);
    return o;
}

void PSMain(SMPSInput p) {}
