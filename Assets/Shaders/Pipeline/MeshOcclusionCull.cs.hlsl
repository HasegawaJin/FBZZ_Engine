/// @file    MeshOcclusionCull.cs.hlsl
/// @brief   通常メッシュの遮蔽を検査し、描画引数を生成する。
/// @author  Hasegawa Jin
/// @date    2026-11-12
#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Pipeline/HiZOcclusion.hlsli"

struct Candidate
{
    float4 bounds;
    uint indexCount;
    uint3 padding;
};

/// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_draw_indexed_arguments
struct IndexedArguments
{
    uint indexCount;
    uint instanceCount;
    uint startIndex;
    int baseVertex;
    uint startInstance;
};

cbuffer OcclusionView : register(b0)
{
    float4x4 viewProjection;
    uint2 size;
    uint candidateCount;
    uint padding;
};

FBZZ_SBUFFER_T(Candidate, g_candidates, SB_SKIN_SRC_VERTICES_SLOT);
FBZZ_SBUFFER_T(float, g_hierarchy, 28);
FBZZ_RWSBUFFER_T(IndexedArguments, g_arguments, UAV_CLUSTER_LIGHTS_SLOT);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= candidateCount) return;
    const Candidate candidate = g_candidates[id.x];
    IndexedArguments args = (IndexedArguments)0;
    args.indexCount = candidate.indexCount;
    args.instanceCount = OccludedByHiZ(candidate.bounds.xyz, candidate.bounds.w, viewProjection, size, g_hierarchy) ? 0 : 1;
    g_arguments[id.x] = args;
}
