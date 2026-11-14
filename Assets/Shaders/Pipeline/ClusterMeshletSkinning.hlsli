/// @file    ClusterMeshletSkinning.hlsli
/// @brief   Reads the standard skinned vertex and palette from a mesh shader.
/// @author  Hasegawa Jin
/// @date    2026-11-13
#ifndef FBZZ_CLUSTER_MESHLET_SKINNING_HLSLI
#define FBZZ_CLUSTER_MESHLET_SKINNING_HLSLI
#include "Common/Structs.hlsli"
#include "Common/Math.hlsli"

FBZZ_VS_SBUFFER(float4x4, g_clusterBones, VS_SB_INSTANCE_SLOT);

float4x4 ClusterBone(uint bone)
{
    if (objectParams.z > 0.5f) return g_clusterBones[uint(objectParams.w) + bone];
    return boneMatrices[bone];
}

float4x4 ClusterSkinMatrix(uint offset)
{
    const uint4 bones = g_meshletRawVertices.Load4(offset + 44u);
    const float4 weights = asfloat(g_meshletRawVertices.Load4(offset + 60u));
    return ClusterBone(bones.x) * weights.x + ClusterBone(bones.y) * weights.y
        + ClusterBone(bones.z) * weights.z + ClusterBone(bones.w) * weights.w;
}

void ApplyClusterSkinning(inout VSInput input, uint offset)
{
    if (g_clusterVertexSkinning == 0u) return;
    const float4x4 skin = ClusterSkinMatrix(offset);
    input.position = mul(float4(input.position, 1.0f), skin).xyz;
    input.normal = SafeNormalize(mul(input.normal, (float3x3)skin), float3(0.0f, 1.0f, 0.0f));
    input.tangent = SafeNormalize(mul(input.tangent, (float3x3)skin), float3(1.0f, 0.0f, 0.0f));
}

#endif /// @note FBZZ_CLUSTER_MESHLET_SKINNING_HLSLI
