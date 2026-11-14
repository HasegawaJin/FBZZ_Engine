/// @file    ShadowMapMesh.ms.hlsl
/// @brief   Static meshlet path that reuses the standard shadow vertex evaluation and depth pixel shader.
/// @author  Hasegawa Jin
/// @date    2026-10-10
/// @see https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html - mesh output limits and SetMeshOutputCounts.

#ifdef FBZZ_MASKED_SHADOW_MESH
#include "Pipeline/Shadow/AlphaShadow.hlsl"
#define FBZZ_SHADOW_MESH_OUTPUT ShadowOutput
#else
#include "Pipeline/Shadow/ShadowMap.hlsl"
#define FBZZ_SHADOW_MESH_OUTPUT SMPSInput
#endif
#define FBZZ_STATIC_CLUSTER_LOD
#include "Pipeline/ClusterMeshletVisibility.hlsli"
#include "Pipeline/ClusterMeshletSkinning.hlsli"

groupshared uint g_meshletId;
groupshared uint g_instanceId;
groupshared bool g_meshletVisible;

/// @brief Emits one meshlet's source positions and original-order local triangles into the light view.
/// @note All lanes reach one uniform SetMeshOutputCounts call; zero counts reject invalid and culled meshlets before vertex loads.
[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void MSMain(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID,
    out vertices FBZZ_SHADOW_MESH_OUTPUT vertices[kFbzzMeshletMaximumVertices],
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
        const uint index = LoadFbzzMeshletGlobalVertex(descriptor, laneIndex);
        const uint offset = index * g_meshletVertexStride;
        float3 position = LoadFbzzMeshletPosition(index);
        if (g_clusterVertexSkinning != 0u)
            position = mul(float4(position, 1.0f), ClusterSkinMatrix(offset)).xyz;
        float4x4 objectWorld = world;
        [branch] if (g_clusterInstanceMode != 0u) objectWorld = g_clusterInstances[g_instanceId].world;
#ifdef FBZZ_MASKED_SHADOW_MESH
        const float2 uv = asfloat(g_meshletRawVertices.Load2(offset + 36u));
        vertices[laneIndex] = EvaluateAlphaShadow(position, uv, objectWorld);
#else
        SMVSInput input;
        input.position = position;
        vertices[laneIndex] = ShadowMapVS(input, objectWorld);
#endif
    }

    for (uint primitiveIndex = laneIndex;
         primitiveIndex < descriptor.primitiveCount;
         primitiveIndex += 64u)
    {
        primitives[primitiveIndex] = LoadFbzzMeshletLocalTriangle(descriptor, primitiveIndex);
    }
}
