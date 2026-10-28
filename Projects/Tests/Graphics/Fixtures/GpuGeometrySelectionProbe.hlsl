/// @file    GpuGeometrySelectionProbe.hlsl
/// @brief   GPU geometry selection indexed-indirect test shader.
/// @author  Hasegawa Jin
/// @date    2026-10-10

struct ProbeInstance
{
    float2 offset;
    float2 padding;
    float4 color;
};

struct ProbeVertexInput
{
    float3 position : POSITION;
};

struct ProbeVertexOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

/// @note Test shader roots do not resolve nested Common includes; packed uint4 lengths match 32 pixel, 4 vertex, and 8 UAV slots.
cbuffer BindlessIndicesConstants : register(b14)
{
    uint4 gBindlessPixel[8];
    uint4 gBindlessVertex[1];
    uint4 gBindlessUav[2];
};
static StructuredBuffer<ProbeInstance> g_probeInstances = ResourceDescriptorHeap[
    NonUniformResourceIndex(gBindlessVertex[0].x)];
cbuffer GeometryInstanceOffsetConstants : register(b15)
{
    uint g_geometryInstanceOffset;
};

ProbeVertexOutput VSMain(ProbeVertexInput input, uint instanceId : SV_InstanceID)
{
    instanceId += g_geometryInstanceOffset;
    const ProbeInstance instance = g_probeInstances[instanceId];
    ProbeVertexOutput output;
    float3 position = input.position;
    position.xy += instance.offset;
    output.position = float4(position, 1.0f);
    output.color = instance.color;
    return output;
}

float4 PSMain(ProbeVertexOutput input) : SV_Target0
{
    return input.color;
}
