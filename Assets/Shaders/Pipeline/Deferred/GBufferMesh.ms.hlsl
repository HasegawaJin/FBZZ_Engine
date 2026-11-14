/// @file    GBufferMesh.ms.hlsl
/// @brief   Static meshlet path that reuses the standard GBuffer vertex evaluation and pixel shader.
/// @author  Hasegawa Jin
/// @date    2026-10-10
/// @see https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html - mesh output limits and SetMeshOutputCounts.

#include "Pipeline/Deferred/GBuffer.hlsl"
#define FBZZ_STATIC_CLUSTER_LOD
#include "Pipeline/ClusterMeshletVisibility.hlsli"
#include "Pipeline/ClusterMeshletSkinning.hlsli"

groupshared uint g_meshletId;
groupshared uint g_instanceId;
groupshared bool g_meshletVisible;

/// @brief Emits at most 64 shared source vertices and 126 original-order local triangles for one meshlet.
/// @note All lanes reach one uniform SetMeshOutputCounts call; zero counts reject invalid and culled meshlets before vertex loads.
[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void MSMain(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID,
    out vertices PSInput vertices[kFbzzMeshletMaximumVertices],
    out indices uint3 primitives[kFbzzMeshletMaximumPrimitives])
{
    if (groupThreadId.x == 0u) {
        const uint2 pair = ReadClusterMeshGroup(groupId);
        g_meshletId = pair.x;
        g_instanceId = pair.y;
        g_meshletVisible = pair.x < g_meshletCount && groupId.z == 0u
            && (FbzzVertexSlot(3u) != 0xffffffffu
                || (groupId.y == 0u && IsClusterMeshletVisible(ReadClusterMeshlet(pair.x), world)));
    }
    GroupMemoryBarrierWithGroupSync();

    FbzzMeshletDescriptor descriptor = (FbzzMeshletDescriptor)0;
    if (g_meshletVisible) descriptor = ReadClusterMeshlet(g_meshletId);
    const bool visible = g_meshletVisible && descriptor.vertexCount <= kFbzzMeshletMaximumVertices
        && descriptor.primitiveCount <= kFbzzMeshletMaximumPrimitives;
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
        ApplyClusterSkinning(input, byteOffset);
        float4x4 objectWorld = world;
        float4x4 objectNormal = worldInvTranspose;
        if (g_clusterInstanceMode != 0u) {
            const ClusterInstanceRow instance = g_clusterInstances[g_instanceId];
            objectWorld = instance.world;
            objectNormal = instance.normal;
        }
        vertices[laneIndex] = GBufferVS(input, objectWorld, objectNormal);
    }

    for (uint primitiveIndex = laneIndex;
         primitiveIndex < descriptor.primitiveCount;
         primitiveIndex += 64u)
    {
        primitives[primitiveIndex] = LoadFbzzMeshletLocalTriangle(descriptor, primitiveIndex);
    }
}
