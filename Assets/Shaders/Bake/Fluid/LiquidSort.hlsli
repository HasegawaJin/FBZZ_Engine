/// @file    LiquidSort.hlsli
/// @brief   GPU 液体: (セル番号, 粒子番号) の bitonic sort の共通部 (ParticleGpuSort* と同じ作り)
/// @author  Hasegawa Jin
/// @date    2026-09-12
#ifndef LIQUID_SORT_HLSLI
#define LIQUID_SORT_HLSLI

#include "Bake/Fluid/LiquidCommon.hlsli"

// FluidGpuLiquidPack.hpp の kGpuLiquidSortBlock と一致させること。
#define LIQUID_SORT_BLOCK 256

RWStructuredBuffer<uint2> gSortEntries : register(u3);

// WHY 粒子番号まで比べるか: 同じセルの中の順番まで決めておくと、近傍の和を足す順が毎回同じになり、焼き直しても同じ絵になる。
bool LiquidSortGreater(uint2 a, uint2 b)
{
    return a.x != b.x ? a.x > b.x : a.y > b.y;
}

// 昇順の区間 (i & k == 0) では大きい方を後ろへ、降順の区間では前へ。
bool LiquidSortShouldSwap(uint2 a, uint2 b, bool ascending)
{
    return LiquidSortGreater(a, b) == ascending;
}

#endif // LIQUID_SORT_HLSLI
