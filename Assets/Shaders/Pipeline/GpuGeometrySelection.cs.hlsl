/// @file    GpuGeometrySelection.cs.hlsl
/// @brief   ビュー別候補の保守的な AABB カリングと投影セル幅 LOD、DrawIndexed 引数生成。
/// @author  Hasegawa Jin
/// @date    2026-10-10

#include "Common/BindlessIndices.hlsli"

struct GpuGeometryLodRange
{
    uint indexCount;
    uint startIndex;
    int baseVertex;
    uint vertexCount;
    float cellSizeWorld;
    float3 padding;
};

struct GpuGeometryCandidate
{
    float4 boundsMin;
    float4 boundsMax;
    uint lodCount;
    uint3 padding;
    GpuGeometryLodRange lods[4];
};

struct GpuGeometrySelectionView
{
    float4 frustumPlanes[6];
    float4 cameraAndProjectionScale;
    float4 projectionMetrics;
    uint candidateCount;
    uint3 padding;
};

struct GpuGeometryDecision
{
    uint visible;
    uint lodIndex;
    uint indexCount;
    uint candidateIndex;
};

/// @brief 1 uint root constant と D3D12_DRAW_INDEXED_ARGUMENTS を連結した 24 byte の indirect record。
/// @note instanceOffset は b15 へ渡し、SV_InstanceID の base には依存しない。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_draw_indexed_arguments - D3D12_DRAW_INDEXED_ARGUMENTS.
struct GpuGeometryIndexedArguments
{
    uint instanceOffset;
    uint indexCountPerInstance;
    uint instanceCount;
    uint startIndexLocation;
    int baseVertexLocation;
    uint startInstanceLocation;
};

#if BINDLESS_SUPPORTED
FBZZ_SBUFFER_T(GpuGeometryCandidate, g_candidates, SB_SKIN_SRC_VERTICES_SLOT);
FBZZ_SBUFFER_T(GpuGeometrySelectionView, g_view, SB_SKIN_BONES_SLOT);
FBZZ_RWSBUFFER_T(GpuGeometryDecision, g_decisions, UAV_CLUSTER_LIGHTS_SLOT);
FBZZ_RWSBUFFER_T(GpuGeometryIndexedArguments, g_indexedArguments, UAV_GPU_SORT_SLOT);
#else
StructuredBuffer<GpuGeometryCandidate> g_candidates : register(t14);
StructuredBuffer<GpuGeometrySelectionView> g_view : register(t15);
RWStructuredBuffer<GpuGeometryDecision> g_decisions : register(u2);
RWStructuredBuffer<GpuGeometryIndexedArguments> g_indexedArguments : register(u3);
#endif

static const uint kGpuGeometryGroupSize = 64;

bool IsCandidateVisible(GpuGeometryCandidate candidate, GpuGeometrySelectionView view)
{
    const float3 boundsMin = candidate.boundsMin.xyz;
    const float3 boundsMax = candidate.boundsMax.xyz;
    [unroll]
    for (uint planeIndex = 0; planeIndex < 6; ++planeIndex)
    {
        const float4 plane = view.frustumPlanes[planeIndex];
        const float3 positiveVertex = float3(
            plane.x >= 0.0f ? boundsMax.x : boundsMin.x,
            plane.y >= 0.0f ? boundsMax.y : boundsMin.y,
            plane.z >= 0.0f ? boundsMax.z : boundsMin.z);
        if (dot(plane.xyz, positiveVertex) + plane.w < 0.0f)
            return false;
    }
    return true;
}

uint SelectProjectedLod(GpuGeometryCandidate candidate, GpuGeometrySelectionView view)
{
    const float3 cameraPosition = view.cameraAndProjectionScale.xyz;
    const float3 nearestPoint = clamp(cameraPosition, candidate.boundsMin.xyz, candidate.boundsMax.xyz);
    const float distanceToBounds = max(length(nearestPoint - cameraPosition), view.projectionMetrics.z);
    const bool orthographic = view.projectionMetrics.w > 0.5f;
    const float pixelsPerWorld = orthographic
        ? view.projectionMetrics.x
        : view.cameraAndProjectionScale.w / distanceToBounds;
    const float targetCellPixels = view.projectionMetrics.y;

    uint selectedLod = 0;
    [loop]
    for (int lodIndex = (int)candidate.lodCount - 1; lodIndex >= 0; --lodIndex)
    {
        const float projectedCellPixels = candidate.lods[lodIndex].cellSizeWorld * pixelsPerWorld;
        if (projectedCellPixels <= targetCellPixels)
        {
            selectedLod = (uint)lodIndex;
            break;
        }
    }
    return selectedLod;
}

/// @brief 1 dispatch thread writes one decision and one ordered 24 byte offset-plus-indexed-draw record.
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing - ExecuteIndirect argument records.
[numthreads(kGpuGeometryGroupSize, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint candidateIndex = dispatchThreadId.x;
    const GpuGeometrySelectionView view = g_view[0];
    if (candidateIndex >= view.candidateCount)
        return;
    const GpuGeometryCandidate candidate = g_candidates[candidateIndex];
    const uint visible = IsCandidateVisible(candidate, view) ? 1u : 0u;
    const uint lodIndex = SelectProjectedLod(candidate, view);
    const GpuGeometryLodRange lod = candidate.lods[lodIndex];

    GpuGeometryDecision decision;
    decision.visible = visible;
    decision.lodIndex = lodIndex;
    decision.indexCount = lod.indexCount;
    decision.candidateIndex = candidateIndex;
    g_decisions[candidateIndex] = decision;

    GpuGeometryIndexedArguments arguments;
    arguments.instanceOffset = candidateIndex;
    arguments.indexCountPerInstance = lod.indexCount;
    arguments.instanceCount = visible;
    arguments.startIndexLocation = lod.startIndex;
    arguments.baseVertexLocation = lod.baseVertex;
    arguments.startInstanceLocation = 0u;
    g_indexedArguments[candidateIndex] = arguments;
}
