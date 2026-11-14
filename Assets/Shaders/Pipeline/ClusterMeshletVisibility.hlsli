/// @file    ClusterMeshletVisibility.hlsli
/// @brief   Visibility shared by direct mesh draws and GPU candidate compaction.
/// @author  Hasegawa Jin
/// @date    2026-11-13
#ifndef FBZZ_CLUSTER_MESHLET_VISIBILITY_HLSLI
#define FBZZ_CLUSTER_MESHLET_VISIBILITY_HLSLI
#include "Pipeline/MeshletGeometry.hlsli"
#include "Pipeline/HiZOcclusion.hlsli"

/// @note Matches the 208-byte resident placement record used by instance visibility.
struct ClusterInstanceRow { float4x4 world, normal, previousWorld; float4 bounds; };
static const uint CLUSTER_INSTANCE_DISPATCH_WIDTH = 256u;

#ifdef FBZZ_CLUSTER_COMPUTE
FBZZ_SBUFFER_T(float, g_clusterHiZ, 29);
FBZZ_SBUFFER_T(ClusterInstanceRow, g_clusterInstances, 15);
FBZZ_SBUFFER_T(uint, g_clusterInstanceIds, 29);
/// @note Matches the 24-byte indexed instance command with the leading ID offset.
struct ClusterIndexedArguments { uint idOffset, indexCount, instanceCount, startIndex; int baseVertex; uint startInstance; };
FBZZ_SBUFFER_T(ClusterIndexedArguments, g_clusterIndexedArguments, 30);
#else
FBZZ_SBUFFER_T(float, g_clusterHiZ, 15);
FBZZ_VS_SBUFFER(ClusterInstanceRow, g_clusterInstances, VS_SB_INSTANCE_SLOT);

uint2 ReadClusterMeshGroup(uint3 groupId)
{
    if (g_clusterInstanceMode != 0u) {
        const uint index = groupId.y * CLUSTER_INSTANCE_DISPATCH_WIDTH + groupId.x;
        if (index >= g_clusterSelection[0]) return uint2(0xffffffffu, 0xffffffffu);
        return uint2(g_clusterSelection[index * 2u + 1u], g_clusterSelection[index * 2u + 2u]);
    }
    [branch] if (FbzzVertexSlot(3u) != 0xffffffffu)
        return uint2(g_clusterSelection[groupId.x], 0xffffffffu);
    return uint2(groupId.x, 0xffffffffu);
}
#endif

FbzzMeshletDescriptor ReadClusterMeshlet(uint index)
{
    const uint offset = index * 48u;
    const uint4 range = g_meshletRawData.Load4(offset + 32u);
    FbzzMeshletDescriptor descriptor;
    descriptor.boundsMin = asfloat(g_meshletRawData.Load4(offset));
    descriptor.boundsMax = asfloat(g_meshletRawData.Load4(offset + 16u));
    descriptor.vertexOffsetBytes = range.x;
    descriptor.primitiveOffsetBytes = range.y;
    descriptor.vertexCount = range.z;
    descriptor.primitiveCount = range.w;
    return descriptor;
}

bool IsClusterMeshletVisible(FbzzMeshletDescriptor descriptor, float4x4 objectWorld)
{
    if (descriptor.vertexCount == 0u || descriptor.vertexCount > kFbzzMeshletMaximumVertices
        || descriptor.primitiveCount == 0u || descriptor.primitiveCount > kFbzzMeshletMaximumPrimitives) return false;
    if (g_clusterNodeCount != 0u) {
        const uint row = g_clusterNodeCount + asuint(descriptor.boundsMax.w) * 3u;
        if (g_clusterPageTable[row] == 0xffffffffu) return false;
    }
    if (!IsFbzzClusterSelected(asuint(descriptor.boundsMin.w), objectWorld) || !IsFbzzMeshletVisible(descriptor, objectWorld))
        return false;
    if (any(g_clusterHiZSize == 0u)) return true;
    const float3 center = (descriptor.boundsMin.xyz + descriptor.boundsMax.xyz) * 0.5f;
    const float3 extent = (descriptor.boundsMax.xyz - descriptor.boundsMin.xyz) * 0.5f + g_clusterWindPadding;
    const float3 worldCenter = mul(float4(center, 1.0f), objectWorld).xyz;
    const float3 worldExtent = extent.x * abs(objectWorld[0].xyz)
        + extent.y * abs(objectWorld[1].xyz) + extent.z * abs(objectWorld[2].xyz);
    const float guard = max(1.0e-5f, length(abs(worldCenter) + worldExtent) * 1.0e-6f);
    return !OccludedByHiZ(worldCenter, length(worldExtent) + guard, viewProjection, g_clusterHiZSize, g_clusterHiZ);
}

#endif /// @note FBZZ_CLUSTER_MESHLET_VISIBILITY_HLSLI
