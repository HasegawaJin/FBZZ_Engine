// FBZZ Engine
// Pipeline/Mask/ObjectMaskSkinned.hlsl | fbzz::renderer
// GPU スキニングされたメッシュのオブジェクトマスク

#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Structs.hlsli"
#include "Common/BindlessIndices.hlsli"

cbuffer ObjectMaskConstants : register(CB_MATERIAL)
{
    float4 objectMaskPayload;
    float4 objectMaskFlags;    // x = visibleOnly
};

// 遮蔽の判定はここで済ませる (ObjectMask.hlsl と同じ理由)。
FBZZ_TEX2D_T(float, texSceneDepth, TEX_DEPTH_SLOT);
static const float kObjectMaskDepthBias = 0.00002f;

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
    if (objectMaskFlags.x > 0.5f) {
        const float sceneDepth = texSceneDepth.Load(int3((int2)input.position.xy, 0));
        if (input.position.z > sceneDepth + kObjectMaskDepthBias) discard;
    }
    return objectMaskPayload;
}
