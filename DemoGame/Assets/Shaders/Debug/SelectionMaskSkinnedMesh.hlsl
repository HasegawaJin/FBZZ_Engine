// FBZZ Engine
// Debug/SelectionMaskSkinnedMesh.hlsl | fbzz::renderer
// Editor selection mask for GPU-skinned meshes

#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"

struct MaskPSInput
{
    float4 position : SV_POSITION;
};

float4x4 BlendSkinMatrix(SkinnedVSInput v)
{
    return boneMatrices[v.boneIndices.x] * v.boneWeights.x
         + boneMatrices[v.boneIndices.y] * v.boneWeights.y
         + boneMatrices[v.boneIndices.z] * v.boneWeights.z
         + boneMatrices[v.boneIndices.w] * v.boneWeights.w;
}

MaskPSInput VSMain(SkinnedVSInput input)
{
    MaskPSInput output;
    float4x4 skinMatrix = BlendSkinMatrix(input);
    float3 localPos = mul(float4(input.position, 1.0f), skinMatrix).xyz;
    float3 worldPos = mul(float4(localPos, 1.0f), world).xyz;
    output.position = mul(float4(worldPos, 1.0f), viewProjection);
    return output;
}

float4 PSMain(MaskPSInput input) : SV_TARGET
{
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
