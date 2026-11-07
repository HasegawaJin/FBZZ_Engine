/// @file    InstanceLodCull.cs.hlsl
/// @brief   Persistent instance visibility and LOD produce IDs and part draw counts without readback.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include "Common/Binding.hlsli"
#include "Common/BindlessIndices.hlsli"
struct Record { float4x4 world; float4x4 normal; float4x4 previousWorld; float4 bounds; };
struct View {
    float4 planes[6];
    float4 cameraProjection, metrics, heights;
    uint4 counts, firstParts;
    float4 forward, culling;
    float4x4 terrainViewProjection;
    uint4 terrainDepth;
    uint4 vegetation;
};
struct Args { uint offset; uint indexCount; uint instanceCount; uint startIndex; int baseVertex; uint startInstance; };
FBZZ_SBUFFER_T(Record,g_records,SB_SKIN_SRC_VERTICES_SLOT);
FBZZ_SBUFFER_T(View,g_view,SB_SKIN_BONES_SLOT);
FBZZ_SBUFFER_T(uint,g_partLods,SB_PARTICLE_FORCES_SLOT);
static const uint VEGETATION_SLOTS = 27u;
FBZZ_SBUFFER_T(uint,g_vegetationSlots,VEGETATION_SLOTS);
FBZZ_RWSBUFFER_T(uint,g_ids,UAV_CLUSTER_LIGHTS_SLOT);
FBZZ_RWSBUFFER_T(Args,g_args,UAV_GPU_SORT_SLOT);
#if !defined(FBZZ_INSTANCE_CASCADE_CULL)
FBZZ_SBUFFER_T(float4,g_occlusion,SB_CLUSTER_INDICES_SLOT);
static const uint TERRAIN_HIZ_SLOT = 28u;
FBZZ_SBUFFER_T(float,g_terrainHiZ,TERRAIN_HIZ_SLOT);
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
/// @brief Tests all tiles covering a projected sphere against the current reversed-Z depth hierarchy.
/// @note Bounds are expanded for clip-space arithmetic and raster sample positions. Near and viewport intersections keep the candidate.
/// @see https://developer.nvidia.com/gpugems/gpugems2/part-v-image-oriented-computing/chapter-42-conservative-rasterization Conservative Depth.
bool OccludedByTerrainHiZ(float3 center, float radius, View view)
{
    if (radius <= 0 || !all(isfinite(float4(center, radius)))) return false;
    const float4 position = float4(center, 1);
    const float4 clipCenter = mul(position, view.terrainViewProjection);
    const float4 clipX = radius * view.terrainViewProjection[0];
    const float4 clipY = radius * view.terrainViewProjection[1];
    const float4 clipZ = radius * view.terrainViewProjection[2];
    const float4 error = 2.0e-6 * (mul(abs(position), abs(view.terrainViewProjection))
        + abs(clipX) + abs(clipY) + abs(clipZ)) + 1.0e-7;
    if (!all(isfinite(error))) return false;

    float2 low = 1.0, high = -1.0;
    float nearest = 0.0;
    [unroll] for (uint corner = 0; corner < 8; ++corner) {
        const float4 clip = clipCenter + ((corner & 1) ? clipX : -clipX)
            + ((corner & 2) ? clipY : -clipY) + ((corner & 4) ? clipZ : -clipZ);
        const float minW = clip.w - error.w;
        const float maxZ = clip.z + error.z;
        if (!all(isfinite(clip)) || minW <= 0 || clip.z - error.z <= 0 || maxZ >= minW) return false;
        const float2 ndc = clip.xy / clip.w;
        const float2 margin = (error.xy + abs(ndc) * error.w) / minW;
        low = min(low, ndc - margin);
        high = max(high, ndc + margin);
        nearest = max(nearest, maxZ / minW);
    }

    const float2 dimensions = float2(view.terrainDepth.xy);
    const float2 firstPixel = float2(low.x * 0.5 + 0.5, 0.5 - high.y * 0.5) * dimensions - 1.0;
    const float2 lastPixel = float2(high.x * 0.5 + 0.5, 0.5 - low.y * 0.5) * dimensions + 1.0;
    if (!all(isfinite(float4(firstPixel, lastPixel))) || any(firstPixel < 0) || any(lastPixel >= dimensions)) return false;

    /// @note Levels are tightly packed, starting at ceil(viewport/4) with ceil-halving to 1×1.
    uint2 tiles = (view.terrainDepth.xy + 3u) / 4u;
    uint2 first = uint2(floor(firstPixel)) / 4u;
    uint2 last = uint2(floor(lastPixel)) / 4u;
    uint levelOffset = 0u;
    [loop] while (any(last - first > 1u)) {
        levelOffset += tiles.x * tiles.y;
        tiles = (tiles + 1u) / 2u;
        first /= 2u;
        last /= 2u;
    }
    /// @note Min reduction preserves the farthest covered pixel, including the far clear depth of holes.
    const float guardedNearest = nearest + 1.0e-5;
    [loop] for (uint y = first.y; y <= last.y; ++y) {
        [loop] for (uint x = first.x; x <= last.x; ++x) {
            const float depth = g_terrainHiZ[levelOffset + y * tiles.x + x];
            if (!isfinite(depth) || depth <= guardedNearest || depth > 1.0) return false;
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
    /// @note 錐台だけがカスケードごとに異なる。距離と LOD は全スライスで同じカメラを使う。
    uint cascadeMask = 0u;
    const uint cascadeCount = min(view.terrainDepth.w, 4u);
    [loop] for (uint cascade=0;cascade<cascadeCount;++cascade)
        if (IntersectsFrustum(g_view[cascade], sphere, radius)) cascadeMask |= 1u << cascade;
    if (cascadeMask == 0u) return;
#else
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
    if (view.terrainDepth.x!=0 && OccludedByTerrainHiZ(sphere.xyz,radius,view)) return;
#endif
    const float relativeHeight=2*sphere.w*view.cameraProjection.w/view.metrics.x
        /(view.metrics.w>0.5f ? 1.0f : max(distance-sphere.w,view.metrics.y));
    uint lod=view.counts.y-1;
    [loop] for (uint i=0;i<view.counts.y;++i) if (relativeHeight>=view.heights[i]) { lod=i; break; }
    const uint last=view.counts.y-1u;
    if (view.vegetation.z!=0u && rank>=view.vegetation.z) lod=last;
    else if (view.vegetation.x!=0u && rank>=view.vegetation.x) lod=max(lod,min(2u,last));
    else if (view.vegetation.y!=0u && rank>=view.vegetation.y) lod=max(lod,min(1u,last));
#if defined(FBZZ_INSTANCE_CASCADE_CULL)
    [loop] while (cascadeMask != 0u) {
        AppendInstance(view, firstbitlow(cascadeMask), id, lod);
        cascadeMask &= cascadeMask - 1u;
    }
#else
    AppendInstance(view, 0u, id, lod);
#endif
}
