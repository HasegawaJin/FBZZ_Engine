/// @file    LiquidNeighbors.hlsli
/// @brief   GPU 液体: 粒子ごとの近傍 9 行 (27 セル) の範囲 (LiquidRanges が刻みの頭に作る)
/// @author  Hasegawa Jin
/// @date    2026-09-12
#ifndef LIQUID_NEIGHBORS_HLSLI
#define LIQUID_NEIGHBORS_HLSLI

#include "Bake/Fluid/LiquidGrid.hlsli"
#include "Common/BindlessIndices.hlsli"

// 粒子 i の近傍は gLiquidRanges[i × 9 + 行] の [x, y) を gLiquidSorted で引いた粒子 (自分も含む)。
#define LIQUID_NEIGHBOR_ROWS 9u

FBZZ_SBUFFER_T(uint2, gLiquidRanges, 29);

#endif // LIQUID_NEIGHBORS_HLSLI
