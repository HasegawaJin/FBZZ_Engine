/// @file    GpuGeometrySelectionProbe.ms.hlsl
/// @brief   GPU geometry selection mesh-equivalence test shader.
/// @author  Hasegawa Jin
/// @date    2026-10-10
/// @see https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html - output topology and SetMeshOutputCounts.

struct ProbeInstance
{
    float2 offset;
    float2 padding;
    float4 color;
};

struct ProbeMeshVertex
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

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void MSMain(uint3 groupId : SV_GroupID,
    out vertices ProbeMeshVertex vertices[3],
    out indices uint3 triangles[1])
{
    SetMeshOutputCounts(3, 1);
    const ProbeInstance instance = g_probeInstances[groupId.y];
    const float3 positions[3] = {
        float3(-0.16f, -0.16f, 0.5f),
        float3(0.0f, 0.16f, 0.5f),
        float3(0.16f, -0.16f, 0.5f)
    };
    [unroll]
    for (uint vertexIndex = 0; vertexIndex < 3; ++vertexIndex)
    {
        float3 position = positions[vertexIndex];
        position.xy += instance.offset;
        vertices[vertexIndex].position = float4(position, 1.0f);
        vertices[vertexIndex].color = instance.color;
    }
    triangles[0] = uint3(0, 1, 2);
}

float4 PSMain(ProbeMeshVertex input) : SV_Target0
{
    return input.color;
}
