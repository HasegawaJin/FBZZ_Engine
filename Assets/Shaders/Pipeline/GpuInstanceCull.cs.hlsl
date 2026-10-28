/// @file    GpuInstanceCull.cs.hlsl
/// @brief   Opaque instance sphere culling, compaction and GPU draw-count generation.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"
struct CullInstance { float4x4 world; float4x4 worldInvTranspose; float4 bounds; };
struct ObjectInstance { float4x4 world; float4x4 worldInvTranspose; };
struct CullView { float4 planes[6]; uint count; uint3 padding; };
struct IndexedArguments { uint indexCount; uint instanceCount; uint startIndex; int baseVertex; uint startInstance; };
FBZZ_SBUFFER_T(CullInstance, g_inputs, SB_SKIN_SRC_VERTICES_SLOT);
FBZZ_SBUFFER_T(CullView, g_view, SB_SKIN_BONES_SLOT);
FBZZ_RWSBUFFER_T(ObjectInstance, g_output, UAV_CLUSTER_LIGHTS_SLOT);
FBZZ_RWSBUFFER_T(IndexedArguments, g_arguments, UAV_GPU_SORT_SLOT);
/// @note InterlockedAdd assigns unique output slots; dispatch completion precedes indirect draw and SRV reads.
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/interlockedadd - atomic increment semantics.
[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const CullView view = g_view[0];
    if (id.x >= view.count) return;
    const CullInstance input = g_inputs[id.x];
    [unroll] for (uint i = 0; i < 6; ++i)
        if (dot(view.planes[i].xyz, input.bounds.xyz) + view.planes[i].w < -input.bounds.w) return;
    uint slot;
    InterlockedAdd(g_arguments[0].instanceCount, 1, slot);
    ObjectInstance instance;
    instance.world = input.world;
    instance.worldInvTranspose = input.worldInvTranspose;
    g_output[slot] = instance;
}
