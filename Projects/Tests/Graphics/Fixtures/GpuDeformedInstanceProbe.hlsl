/// @file    GpuDeformedInstanceProbe.hlsl
/// @brief   Production raw-vertex descriptors and ordered deformed-instance rows.
/// @author  Hasegawa Jin
/// @date    2026-10-19
#include "Common/DeformedInstance.hlsli"
struct Output { float4 position : SV_POSITION; nointerpolation float4 color : TEXCOORD0; };
Output VSMain(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    Output result;
    result.position = mul(float4(DeformedPosition(vertex,instance),1),gDeformedInstances[instance].world);
    result.color = float4(gDeformedInstances[instance].normal[0].xyz,1);
    return result;
}
float4 PSMain(Output input) : SV_TARGET { return input.color; }
