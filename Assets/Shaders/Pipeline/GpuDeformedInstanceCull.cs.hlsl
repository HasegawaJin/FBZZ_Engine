/// @file    GpuDeformedInstanceCull.cs.hlsl
/// @brief   Stable deformed-instance filtering and GPU indexed-draw counts.
/// @author  Hasegawa Jin
/// @date    2026-10-19
#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"
#ifdef FBZZ_CULL_STATIC
struct Instance { float4x4 world; float4x4 worldInvTranspose; };
#else
struct Instance { float4x4 world; float4x4 worldInvTranspose; uint vertexIndex; uint3 padding; };
#endif
struct CullView { float4 planes[6]; uint count; uint3 padding; };
struct IndexedArguments { uint indexCount; uint instanceCount; uint startIndex; int baseVertex; uint startInstance; };
FBZZ_SBUFFER_T(float4, g_bounds, 13);
FBZZ_SBUFFER_T(Instance, g_inputs, SB_SKIN_SRC_VERTICES_SLOT);
FBZZ_SBUFFER_T(CullView, g_view, SB_SKIN_BONES_SLOT);
FBZZ_RWSBUFFER_T(Instance, g_output, UAV_CLUSTER_LIGHTS_SLOT);
FBZZ_RWSBUFFER_T(IndexedArguments, g_arguments, UAV_GPU_SORT_SLOT);
groupshared uint prefix[64];
groupshared uint completed;
/// @note One group scans consecutive tiles; stable offsets retain primitive order, including equal-depth overlaps.
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/groupmemorybarrierwithgroupsync
[numthreads(64,1,1)]
void CSMain(uint3 laneId : SV_GroupThreadID)
{
    uint lane = laneId.x;
    CullView view = g_view[0];
    if (lane == 0) completed = 0;
    GroupMemoryBarrierWithGroupSync();
    for (uint begin = 0; begin < view.count; begin += 64) {
        uint row = begin + lane;
        bool visible = row < view.count;
        if (visible) {
            float4 sphere = g_bounds[row];
            [unroll] for (uint plane = 0; plane < 6; ++plane)
                if (dot(view.planes[plane].xyz,sphere.xyz)+view.planes[plane].w < -sphere.w) visible = false;
        }
        prefix[lane] = visible ? 1 : 0;
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint offset = 1; offset < 64; offset <<= 1) {
            uint add = lane >= offset ? prefix[lane-offset] : 0;
            GroupMemoryBarrierWithGroupSync();
            prefix[lane] += add;
            GroupMemoryBarrierWithGroupSync();
        }
        if (visible) g_output[completed+prefix[lane]-1] = g_inputs[row];
        GroupMemoryBarrierWithGroupSync();
        if (lane == 0) completed += prefix[63];
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0) { IndexedArguments args = g_arguments[0]; args.instanceCount = completed; g_arguments[0] = args; }
}
