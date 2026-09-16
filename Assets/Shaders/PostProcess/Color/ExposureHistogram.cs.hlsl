/// @file ExposureHistogram.cs.hlsl
/// @brief HDR バッファの対数輝度ヒストグラムを作る (自動露出のパス 1)
/// @author Hasegawa Jin
/// @date 2026-08-25
//
// WHY グループ共有メモリへ一度溜めるか: 1 画素ごとにグローバル UAV へ InterlockedAdd を
//     打つと、同じビンへ数十万スレッドが直列化する。まずグループ内 (16x16 = 256 画素) で
//     畳んでからビンごとに 1 回だけグローバルへ足せば、原子操作の回数が 256 分の 1 になる。

#include "PostProcess/Color/ExposureCommon.hlsli"
#include "Platform/Backend.hlsli"

Texture2D<float4>     gHdr       : register(TEX_GBUFFER0);
RWStructuredBuffer<uint> gHistogram : register(UAV_CLUSTER_LIGHTS); // u2

groupshared uint s_bins[FBZZ_EXPOSURE_BINS];

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    // 1 グループ = 256 スレッド = ビン数なので、1 スレッドが 1 ビンを初期化する。
    s_bins[groupIndex] = 0u;
    GroupMemoryBarrierWithGroupSync();

    uint width, height;
    gHdr.GetDimensions(width, height);
    if (dispatchId.x < width && dispatchId.y < height) {
        const float3 hdr = gHdr.Load(int3(dispatchId.xy, 0)).rgb;
        InterlockedAdd(s_bins[FBZZ_LuminanceToBin(FBZZ_Luminance(hdr))], 1u);
    }

    GroupMemoryBarrierWithGroupSync();

    // 空のビンまでグローバルへ足しに行かない。屋内の暗いシーンでは
    // 明部側のビンがほぼ全て 0 になるので、この判定だけで原子操作が大きく減る。
    if (s_bins[groupIndex] > 0u)
        InterlockedAdd(gHistogram[groupIndex], s_bins[groupIndex]);
}
