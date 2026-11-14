/// @file    ClusterMeshletCompact.cs.hlsl
/// @brief   Packs visible meshlets and emits a single indirect mesh dispatch.
/// @author  Hasegawa Jin
/// @date    2026-11-13
#include "Common/Constants.hlsli"
#define FBZZ_STATIC_CLUSTER_LOD
#define FBZZ_CLUSTER_COMPUTE
#include "Pipeline/ClusterMeshletVisibility.hlsli"

struct MeshDispatchArguments
{
    uint x, y, z;
};

FBZZ_RWSBUFFER_T(uint, g_visibleMeshlets, 2);
FBZZ_RWSBUFFER_T(MeshDispatchArguments, g_meshDispatch, 3);
groupshared uint g_groupOffset;
groupshared uint g_visibleFlags[32];

/// @see https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html ExecuteIndirect mesh dispatch argument layout.
[numthreads(32, 1, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID, uint lane : SV_GroupIndex)
{
    /// @note Groups reserve one contiguous range independently of the device wave width.
    const bool instanced = g_clusterInstanceMode != 0u;
    const uint candidate = instanced ? dispatchId.x % g_meshletCount : dispatchId.x;
    uint instanceId = 0xffffffffu;
    bool visible = false;
    if (instanced) {
        const uint index = dispatchId.x / g_meshletCount;
        uint count = g_clusterInstanceCapacity;
        uint first = 0u;
        if (g_clusterInstanceMode == 2u) {
            const ClusterIndexedArguments source = g_clusterIndexedArguments[g_clusterArgumentIndex];
            count = min(count, source.instanceCount);
            first = source.idOffset;
        }
        if (index < count) {
            instanceId = g_clusterInstanceIds[first + index];
            visible = IsClusterMeshletVisible(ReadClusterMeshlet(candidate), g_clusterInstances[instanceId].world);
        }
    } else if (candidate < g_meshletCount) {
        visible = IsClusterMeshletVisible(ReadClusterMeshlet(candidate), world);
    }
    g_visibleFlags[lane] = visible ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0u) {
        uint count = 0u;
        for (uint i = 0u; i < 32u; ++i) {
            const uint flag = g_visibleFlags[i];
            g_visibleFlags[i] = count;
            count += flag;
        }
        g_groupOffset = 0u;
        if (count != 0u) {
            if (instanced) {
                InterlockedAdd(g_visibleMeshlets[0], count, g_groupOffset);
                InterlockedMax(g_meshDispatch[0].y,
                    (g_groupOffset + count + CLUSTER_INSTANCE_DISPATCH_WIDTH - 1u) / CLUSTER_INSTANCE_DISPATCH_WIDTH);
            } else InterlockedAdd(g_meshDispatch[0].x, count, g_groupOffset);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (visible) {
        const uint index = g_groupOffset + g_visibleFlags[lane];
        if (instanced) {
            g_visibleMeshlets[index * 2u + 1u] = candidate;
            g_visibleMeshlets[index * 2u + 2u] = instanceId;
        } else g_visibleMeshlets[index] = candidate;
    }
}
