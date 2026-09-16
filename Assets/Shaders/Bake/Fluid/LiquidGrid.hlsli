/// @file    LiquidGrid.hlsli
/// @brief   GPU 液体: 並べ替え済み (セル, 粒子) からセルの粒子の範囲を二分探索で引く
/// @author  Hasegawa Jin
/// @date    2026-09-12
//
// セルの開始位置の表は持たない (理由は FluidGpuLiquidSolver.cpp の冒頭)。
// x 方向に隣り合うセルは鍵が連続するので、x の区間 [xLo, xHi] は 1 本の連続した範囲になる。
#ifndef LIQUID_GRID_HLSLI
#define LIQUID_GRID_HLSLI

#include "Bake/Fluid/LiquidCommon.hlsli"

StructuredBuffer<uint2> gLiquidSorted : register(t15); // (セル番号, 粒子番号)。鍵の昇順

// 鍵が key 以上の最初の位置。
uint LiquidLowerBound(uint key)
{
    uint lo = 0u;
    uint hi = gSortCount;
    [loop]
    while (lo < hi)
    {
        const uint mid = (lo + hi) >> 1;
        if (gLiquidSorted[mid].x < key) lo = mid + 1u;
        else hi = mid;
    }
    return lo;
}

// セル (xLo..xHi, y, z) に入っている粒子の、並べ替え済みの範囲 [x, y)。
uint2 LiquidRowRange(int xLo, int xHi, int y, int z)
{
    const uint keyLo = LiquidCellKey(int3(xLo, y, z));
    const uint keyHi = LiquidCellKey(int3(xHi, y, z));
    return uint2(LiquidLowerBound(keyLo), LiquidLowerBound(keyHi + 1u));
}

#endif // LIQUID_GRID_HLSLI
