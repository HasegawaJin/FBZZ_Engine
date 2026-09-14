/// @file    LiquidSortStep.cs.hlsl
/// @brief   GPU 液体: bitonic sort のグローバル 1 段 (比較の相手がグループ幅を超える段)
/// @author  Hasegawa Jin
/// @date    2026-09-12
//
// dispatch: gSortCount / LIQUID_SORT_BLOCK を (k, j) ごとに 1 回。gPassA = k / gPassB = j。
#include "Bake/Fluid/LiquidSort.hlsli"

[numthreads(LIQUID_SORT_BLOCK, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    const uint partner = i ^ gPassB;
    // 各ペアは片側のスレッドだけが処理する (両方が入れ替えると元へ戻る)。
    if (i >= gSortCount || partner <= i || partner >= gSortCount) return;

    const uint2 a = gSortEntries[i];
    const uint2 b = gSortEntries[partner];
    if (LiquidSortShouldSwap(a, b, (i & gPassA) == 0u))
    {
        gSortEntries[i] = b;
        gSortEntries[partner] = a;
    }
}
