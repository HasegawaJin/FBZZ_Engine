/// @file    ObjectMask.hlsl
/// @brief   Render declared silhouette payload with scene-depth visibility.
/// @author  Hasegawa Jin
/// @date    2026-08-28
/// @note b2 belongs to the request payload; the mask target can read scene depth independently.
#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Common/ObjectInstance.hlsli"

cbuffer ObjectMaskConstants : register(CB_MATERIAL)
{
    float4 objectMaskPayload; ///< @note そのまま出力する RGBA
    float4 objectMaskFlags; ///< @note x = visibleOnly
};
/// @note The visible-only comparison uses reversed-Z scene depth and a small self-surface bias.
FBZZ_TEX2D_T(float, texSceneDepth, TEX_DEPTH_SLOT);
static const float kObjectMaskDepthBias = 0.00002f;

struct VSInput
{
    float3 position : POSITION;
};

struct PSInput
{
    float4 position : SV_POSITION;
};
PSInput ObjectMaskVS(VSInput input, float4x4 objectWorld)
{
    PSInput output;
    float3 worldPos = mul(float4(input.position, 1.0f), objectWorld).xyz;
    output.position = mul(float4(worldPos, 1.0f), viewProjection);
    return output;
}

#if defined(FBZZ_DEFORMED_INSTANCED)
#include "Common/DeformedInstance.hlsli"
PSInput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    VSInput input;
    input.position = DeformedPosition(vertexId, instanceId);
    return ObjectMaskVS(input, gDeformedInstances[instanceId].world);
}
#elif defined(FBZZ_INSTANCED)
/// @note 束ねたシルエット。@see Docs/design/gpu-instancing.md
PSInput VSMain(VSInput input, uint instanceId : SV_InstanceID)
{
    return ObjectMaskVS(input, gObjectInstances[instanceId].world);
}
#else
PSInput VSMain(VSInput input)
{
    return ObjectMaskVS(input, world);
}
#endif

float4 PSMain(PSInput input) : SV_TARGET
{
    if (objectMaskFlags.x > 0.5f) {
        const float sceneDepth = texSceneDepth.Load(int3((int2)input.position.xy, 0)); ///< @note カメラ深度は Reversed-Z (手前ほど大きい)。シーンより奥なら捨てる。
        if (input.position.z < sceneDepth - kObjectMaskDepthBias) discard;
    }
    return objectMaskPayload;
}
