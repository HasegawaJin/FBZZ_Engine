/// @file    GpuInstanceProbe.hlsl
/// @brief   Verify production instance-buffer layout after GPU compaction.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#ifdef FBZZ_INSTANCE_DIRECT
#include "Common/Constants.hlsli"
#else
#define FBZZ_INSTANCED 1
#include "Common/ObjectInstance.hlsli"
#endif
struct Output { float4 position : SV_POSITION; nointerpolation float4 color : TEXCOORD0; };
Output VSMain(float3 position : POSITION, uint id : SV_InstanceID)
{
#ifdef FBZZ_INSTANCE_DIRECT
    const float4x4 objectWorld = world;
    const float4x4 objectNormal = worldInvTranspose;
#else
    const ObjectInstance instance = gObjectInstances[id];
    const float4x4 objectWorld = instance.world;
    const float4x4 objectNormal = instance.worldInvTranspose;
#endif
    Output output;
    output.position = mul(float4(position, 1), objectWorld);
    output.color = float4(objectNormal[0].xyz, 1);
    return output;
}
float4 PSMain(Output input) : SV_TARGET { return input.color; }
