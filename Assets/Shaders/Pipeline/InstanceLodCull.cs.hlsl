/// @file    InstanceLodCull.cs.hlsl
/// @brief   Persistent instance visibility and LOD produce IDs and part draw counts without readback.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"
struct Record { float4x4 world; float4x4 normal; float4x4 previousWorld; float4 bounds; };
struct View { float4 planes[6]; float4 cameraProjection; float4 metrics; float4 heights; uint4 counts; uint4 firstParts; float4 forward; float4 culling; };
struct Args { uint offset; uint indexCount; uint instanceCount; uint startIndex; int baseVertex; uint startInstance; };
FBZZ_SBUFFER_T(Record,g_records,SB_SKIN_SRC_VERTICES_SLOT);
FBZZ_SBUFFER_T(View,g_view,SB_SKIN_BONES_SLOT);
FBZZ_SBUFFER_T(uint,g_partLods,SB_PARTICLE_FORCES_SLOT);
FBZZ_RWSBUFFER_T(uint,g_ids,UAV_CLUSTER_LIGHTS_SLOT);
FBZZ_RWSBUFFER_T(Args,g_args,UAV_GPU_SORT_SLOT);
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/interlockedadd - unique visible slots.
[numthreads(64,1,1)]
void CSMain(uint3 threadId : SV_DispatchThreadID)
{
    const View view=g_view[0]; const uint id=threadId.x;
    if (id>=view.counts.x) return;
    const float4 sphere=g_records[id].bounds;
    if (sphere.w<0) return;
    const float radius=sphere.w+view.culling.x;
    if (view.counts.w!=0) {
        [unroll] for (uint p=0;p<6;++p)
            if (dot(view.planes[p].xyz,sphere.xyz)+view.planes[p].w < -radius) return;
    }
    const float3 delta=sphere.xyz-view.cameraProjection.xyz;
    const float distance=length(delta);
    const float cullDistance=view.culling.z>0.5f ? dot(delta,view.forward.xyz) : distance;
    if (view.metrics.z>0 && cullDistance-radius>view.metrics.z) return;
    const float screenHeight=2*radius*view.cameraProjection.w/view.metrics.x
        /(view.metrics.w>0.5f ? 1.0f : max(cullDistance,view.metrics.y));
    if (view.culling.y>0 && cullDistance>0 && screenHeight<view.culling.y) return;
    const float relativeHeight=2*sphere.w*view.cameraProjection.w/view.metrics.x
        /(view.metrics.w>0.5f ? 1.0f : max(distance-sphere.w,view.metrics.y));
    uint lod=view.counts.y-1;
    [loop] for (uint i=0;i<view.counts.y;++i) if (relativeHeight>=view.heights[i]) { lod=i; break; }
    uint slot;
    InterlockedAdd(g_args[view.firstParts[lod]].instanceCount,1,slot);
    g_ids[lod*view.counts.x+slot]=id;
    [loop] for (uint part=0;part<view.counts.z;++part)
        if (part!=view.firstParts[lod] && g_partLods[part]==lod) InterlockedAdd(g_args[part].instanceCount,1);
}
