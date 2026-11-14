/// @file    InstanceLodCull.cs.hlsl
/// @brief   Persistent instance visibility and LOD produce IDs and part draw counts without readback.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Pipeline/HiZOcclusion.hlsli"
struct Record { float4x4 world; float4x4 normal; float4x4 previousWorld; float4 bounds; };
struct View {
    float4 planes[6];
    float4 cameraProjection, metrics, heights;
    uint4 counts, firstParts;
    float4 forward, culling;
    float4x4 terrainViewProjection;
    uint4 terrainDepth;
    uint4 vegetation;
    uint4 shadowViewport;
};
struct Args { uint offset; uint indexCount; uint instanceCount; uint startIndex; int baseVertex; uint startInstance; };
FBZZ_SBUFFER_T(Record,g_records,SB_SKIN_SRC_VERTICES_SLOT);
FBZZ_SBUFFER_T(View,g_view,SB_SKIN_BONES_SLOT);
FBZZ_SBUFFER_T(uint,g_partLods,SB_PARTICLE_FORCES_SLOT);
static const uint VEGETATION_SLOTS = 27u;
FBZZ_SBUFFER_T(uint,g_vegetationSlots,VEGETATION_SLOTS);
FBZZ_RWSBUFFER_T(uint,g_ids,UAV_CLUSTER_LIGHTS_SLOT);
FBZZ_RWSBUFFER_T(Args,g_args,UAV_GPU_SORT_SLOT);
static const uint TERRAIN_HIZ_SLOT = 28u;
FBZZ_SBUFFER_T(float,g_terrainHiZ,TERRAIN_HIZ_SLOT);

bool OccludedByShadowHiZ(const View light, float3 center, float radius)
{
    if (light.terrainDepth.x == 0 || light.shadowViewport.z == 0) return false;
    return OccludedByHiZRegion(center, radius, light.terrainViewProjection, light.terrainDepth.xy,
        uint4(light.shadowViewport.xy, light.shadowViewport.zz), light.shadowViewport.w != 0, g_terrainHiZ);
}

#if !defined(FBZZ_INSTANCE_CASCADE_CULL)
FBZZ_SBUFFER_T(float4,g_occlusion,SB_CLUSTER_INDICES_SLOT);
/// @note Current-view conservative CPU depth, without previous-frame queries or CPU visibility readback.
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3d11/conservative-rasterization Fully covered cells only.
bool Occluded(float3 center, float radius)
{
    const float4 metrics = g_occlusion[8];
    if (metrics.w <= 0 || radius <= 0 || !all(isfinite(float4(center, radius)))) return false;
    const float4 samplePosition = float4(center, 1);
    const float3 viewCenter = float3(dot(g_occlusion[0], samplePosition), dot(g_occlusion[1], samplePosition), dot(g_occlusion[2], samplePosition));
    if (!all(isfinite(viewCenter)) || viewCenter.z - radius <= metrics.x) return false;
    float2 low = 1.0e30, high = -1.0e30;
    [unroll] for (uint corner = 0; corner < 8; ++corner) {
        const float4 vertex = float4(viewCenter + float3((corner & 1) ? radius : -radius,
            (corner & 2) ? radius : -radius, (corner & 4) ? radius : -radius), 1);
        const float w = dot(g_occlusion[7], vertex);
        if (!isfinite(w) || w <= 0) return false;
        const float2 ndc = float2(dot(g_occlusion[4], vertex), dot(g_occlusion[5], vertex)) / w;
        const float2 pixel = (ndc * float2(0.5, -0.5) + 0.5) * metrics.zw;
        if (!all(isfinite(pixel))) return false;
        low = min(low, pixel); high = max(high, pixel);
    }
    /// @note GPU float rounding expands candidates and moves their depth toward the eye.
    low -= 0.01; high += 0.01;
    if (low.x >= metrics.z || low.y >= metrics.w || high.x < 0 || high.y < 0) return false;
    const uint2 first = (uint2)floor(clamp(low, 0, metrics.zw - 1));
    const uint2 last = (uint2)floor(clamp(high, 0, metrics.zw - 1));
    const float nearest = saturate((viewCenter.z - radius - metrics.x) / (metrics.y - metrics.x)) - 1.0e-5;
    [loop] for (uint y = first.y; y <= last.y; ++y) {
        [loop] for (uint x = first.x; x <= last.x; ++x) {
            const uint pixel = y * (uint)metrics.z + x;
            if (g_occlusion[9 + pixel / 4][pixel & 3] >= nearest) return false;
        }
    }
    return true;
}
#endif

bool IntersectsFrustum(const View view, float4 sphere, float radius)
{
    if (view.counts.w!=0) {
        [unroll] for (uint plane=0;plane<6;++plane)
            if (dot(view.planes[plane].xyz,sphere.xyz)+view.planes[plane].w < -radius) return false;
    }
    return true;
}

static const uint SHADOW_CASTER_CULLED = 4u;
/// @note The resident sphere includes leaf motion; generic visibility padding does not change its shadow footprint.
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine#improving-non-nanite-performance Epic, projected-size culling and mesh LOD.
uint ShadowCasterMinimumLod(const View view, float4 sphere)
{
    if (!(view.forward.w > 0.0f) || !(sphere.w > 0.0f) || !all(isfinite(sphere))) return 0u;
    const float diameter = (sphere.w * view.forward.w) * 4.0f;
    if (diameter < 2.0f) return SHADOW_CASTER_CULLED;
    if (diameter <= 8.0f) return 3u;
    if (diameter <= 40.0f) return 2u;
    return diameter <= 80.0f ? 1u : 0u;
}

/// @note 各カスケードが独立した可視 ID・間接引数領域を持つ。通常ビューは常にスライス 0。
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/interlockedadd - unique visible slots.
void AppendInstance(const View view, uint viewIndex, uint id, uint lod)
{
    const uint firstArg = viewIndex * view.counts.z;
    const uint lodArg = view.firstParts[lod];
    uint slot;
    InterlockedAdd(g_args[firstArg+lodArg].instanceCount,1,slot);
    g_ids[(viewIndex*view.counts.y+lod)*view.counts.x+slot]=id;
    [loop] for (uint part=0;part<view.counts.z;++part)
        if (part!=lodArg && g_partLods[part]==lod) InterlockedAdd(g_args[firstArg+part].instanceCount,1);
}

[numthreads(64,1,1)]
void CSMain(uint3 threadId : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
    const View view=g_view[0];
    uint id=threadId.x;
    uint rank=0u;
    [branch] if (view.vegetation.w!=0u) {
        const uint group=groupId.x+65535u*groupId.y;
        if (group>=view.vegetation.w) return;
        /// @note パーツ LOD の直後に、採用セルの開始位置、レーン数、セル内順位を置く。
        const uint descriptor=view.counts.z+3u*group;
        if (lane>=g_partLods[descriptor+1u]) return;
        id=g_vegetationSlots[g_partLods[descriptor]+lane];
        rank=g_partLods[descriptor+2u]+lane;
    } else if (id>=view.counts.x) return;
    const float4 sphere=g_records[id].bounds;
    if (sphere.w<0) return;
    const float radius=sphere.w+view.culling.x;
#if defined(FBZZ_INSTANCE_CASCADE_CULL)
    /// @note Cascades share the camera distance and base LOD; the shadow footprint and frustum are per cascade.
    uint cascadeMask = 0u;
    uint cascadeMinLods = 0u;
    const uint cascadeCount = min(view.terrainDepth.w, 4u);
    [loop] for (uint cascade=0;cascade<cascadeCount;++cascade) {
        const View light = g_view[cascade];
        const uint minimumLod = ShadowCasterMinimumLod(light, sphere);
        if (minimumLod == SHADOW_CASTER_CULLED || !IntersectsFrustum(light, sphere, radius)) continue;
        cascadeMask |= 1u << cascade;
        cascadeMinLods |= minimumLod << (2u * cascade);
    }
    if (cascadeMask == 0u) return;
#else
    const uint minimumLod = ShadowCasterMinimumLod(view, sphere);
    if (minimumLod == SHADOW_CASTER_CULLED) return;
    if (!IntersectsFrustum(view, sphere, radius)) return;
#endif
    const float3 delta=sphere.xyz-view.cameraProjection.xyz;
    const float distance=length(delta);
    const float cullDistance=view.culling.z>0.5f ? dot(delta,view.forward.xyz) : distance;
    if (view.metrics.z>0 && cullDistance-radius>view.metrics.z) return;
    const float screenHeight=2*radius*view.cameraProjection.w/view.metrics.x
        /(view.metrics.w>0.5f ? 1.0f : max(cullDistance,view.metrics.y));
    if (view.culling.y>0 && cullDistance>0 && screenHeight<view.culling.y) return;
#if !defined(FBZZ_INSTANCE_CASCADE_CULL)
    if (view.culling.w>0 && Occluded(sphere.xyz,radius)) return;
    if (OccludedByShadowHiZ(view, sphere.xyz, radius)) return;
    if (view.shadowViewport.z==0 && view.terrainDepth.x!=0 && OccludedByHiZ(sphere.xyz, radius, view.terrainViewProjection,
        view.terrainDepth.xy, g_terrainHiZ)) return;
#endif
    const float relativeHeight=2*sphere.w*view.cameraProjection.w/view.metrics.x
        /(view.metrics.w>0.5f ? 1.0f : max(distance-sphere.w,view.metrics.y));
    uint lod=view.counts.y-1;
    [loop] for (uint i=0;i<view.counts.y;++i) if (relativeHeight>=view.heights[i]) { lod=i; break; }
    const uint last=view.counts.y-1u;
#if defined(FBZZ_INSTANCE_CASCADE_CULL)
    const uint budgetedLod=view.vegetation.z!=0u && rank>=view.vegetation.z ? max(lod,min(2u,last)) : lod;
    /// @note Grouped shadows reuse the terrain Hi-Z slot for the mask of near cascades that preserve detail.
    const uint preserveDetailMask=view.terrainDepth.z;
    [loop] while (cascadeMask != 0u) {
        const uint cascade=firstbitlow(cascadeMask);
        cascadeMask &= cascadeMask - 1u;
        if (OccludedByShadowHiZ(g_view[cascade], sphere.xyz, radius)) continue;
        const uint shadowLod=(preserveDetailMask & (1u<<cascade))!=0u ? lod : budgetedLod;
        const uint minimumLod = (cascadeMinLods >> (2u * cascade)) & 3u;
        AppendInstance(view, cascade, id, max(shadowLod, min(minimumLod, last)));
    }
#else
    /// @note Only the projected-size test can select the distant billboard LOD for shadow casters.
    if (view.vegetation.z!=0u && rank>=view.vegetation.z) lod=max(lod,min(2u,last));
    else if (view.vegetation.x!=0u && rank>=view.vegetation.x) lod=max(lod,min(2u,last));
    else if (view.vegetation.y!=0u && rank>=view.vegetation.y) lod=max(lod,min(1u,last));
    AppendInstance(view, 0u, id, max(lod, min(minimumLod, last)));
#endif
}
