/// @file    GBufferMesh.ms.hlsl
/// @brief   Static meshlet path that reuses the standard GBuffer vertex evaluation and pixel shader.
/// @author  Hasegawa Jin
/// @date    2026-10-10
/// @see https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html - mesh output limits and SetMeshOutputCounts.

#include "Pipeline/Deferred/GBuffer.hlsl"
#include "Pipeline/MeshletGeometry.hlsli"

groupshared uint4 g_meshletDescriptorWords[3];

/// @brief Emits at most 64 shared source vertices and 126 original-order local triangles for one meshlet.
/// @note All lanes reach one uniform SetMeshOutputCounts call; zero counts reject invalid and culled meshlets before vertex loads.
[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void MSMain(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID,
    out vertices PSInput vertices[kFbzzMeshletMaximumVertices],
    out indices uint3 primitives[kFbzzMeshletMaximumPrimitives])
{
    const bool meshletInRange = groupId.x < g_meshletCount && groupId.y == 0u && groupId.z == 0u;
    if (groupThreadId.x == 0u)
    {
        if (meshletInRange)
        {
            const uint byteOffset = groupId.x * 48u;
            g_meshletDescriptorWords[0] = g_meshletRawData.Load4(byteOffset);
            g_meshletDescriptorWords[1] = g_meshletRawData.Load4(byteOffset + 16u);
            g_meshletDescriptorWords[2] = g_meshletRawData.Load4(byteOffset + 32u);
        }
        else
        {
            g_meshletDescriptorWords[0] = 0u;
            g_meshletDescriptorWords[1] = 0u;
            g_meshletDescriptorWords[2] = 0u;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    FbzzMeshletDescriptor descriptor;
    descriptor.boundsMin = asfloat(g_meshletDescriptorWords[0]);
    descriptor.boundsMax = asfloat(g_meshletDescriptorWords[1]);
    descriptor.vertexOffsetBytes = g_meshletDescriptorWords[2].x;
    descriptor.primitiveOffsetBytes = g_meshletDescriptorWords[2].y;
    descriptor.vertexCount = g_meshletDescriptorWords[2].z;
    descriptor.primitiveCount = g_meshletDescriptorWords[2].w;

    const bool vertexLayoutValid = g_meshletPositionOffset <= g_meshletVertexStride
        && g_meshletVertexStride - g_meshletPositionOffset >= 60u;
    const bool descriptorValid = meshletInRange
        && descriptor.vertexCount > 0u
        && descriptor.vertexCount <= kFbzzMeshletMaximumVertices
        && descriptor.primitiveCount > 0u
        && descriptor.primitiveCount <= kFbzzMeshletMaximumPrimitives
        && vertexLayoutValid;
    const bool visible = descriptorValid && IsFbzzMeshletVisible(descriptor, world);
    const uint outputVertexCount = visible ? descriptor.vertexCount : 0u;
    const uint outputPrimitiveCount = visible ? descriptor.primitiveCount : 0u;
    SetMeshOutputCounts(outputVertexCount, outputPrimitiveCount);

    if (!visible)
        return;

    const uint laneIndex = groupThreadId.x;
    if (laneIndex < descriptor.vertexCount)
    {
        const uint sourceVertexIndex = LoadFbzzMeshletGlobalVertex(descriptor, laneIndex);
        const uint byteOffset = sourceVertexIndex * g_meshletVertexStride + g_meshletPositionOffset;
        VSInput input;
        input.position = LoadFbzzMeshletPosition(sourceVertexIndex);
        input.normal = asfloat(g_meshletRawVertices.Load3(byteOffset + 12u));
        input.tangent = asfloat(g_meshletRawVertices.Load3(byteOffset + 24u));
        input.uv = asfloat(g_meshletRawVertices.Load2(byteOffset + 36u));
        vertices[laneIndex] = GBufferVS(input, world, worldInvTranspose);
    }

    for (uint primitiveIndex = laneIndex;
         primitiveIndex < descriptor.primitiveCount;
         primitiveIndex += 64u)
    {
        primitives[primitiveIndex] = LoadFbzzMeshletLocalTriangle(descriptor, primitiveIndex);
    }
}
