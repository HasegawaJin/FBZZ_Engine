/// @file    ClusterMeshletReset.cs.hlsl
/// @brief   Initializes the indirect mesh dispatch before cluster selection.
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
FBZZ_RWSBUFFER_T(MeshDispatchArguments, g_meshDispatch, 3);
FBZZ_RWSBUFFER_T(uint, g_visibleMeshlets, 2);

[numthreads(1, 1, 1)]
void CSMain()
{
    MeshDispatchArguments empty = {0u, 1u, 1u};
    if (g_clusterInstanceMode != 0u) {
        g_visibleMeshlets[0] = 0u;
        empty.x = CLUSTER_INSTANCE_DISPATCH_WIDTH;
        empty.y = 0u;
    }
    g_meshDispatch[0] = empty;
}
