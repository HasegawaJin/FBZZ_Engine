/// @file    LiquidSortLocal.cs.hlsl
/// @brief   GPU 液体: bitonic sort の内側の全段 (j <= LIQUID_SORT_BLOCK / 2) をグループ共有メモリで回す
/// @author  Hasegawa Jin
/// @date    2026-09-12
//
// dispatch: gSortCount / LIQUID_SORT_BLOCK を外側の段 k ごとに 1 回。gPassA = k / gPassB = 最初の j。
#include "Bake/Fluid/LiquidSort.hlsli"

groupshared uint2 gBlock[LIQUID_SORT_BLOCK];

[numthreads(LIQUID_SORT_BLOCK, 1, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID)
{
    const uint global = dispatchId.x;
    const uint local = groupThreadId.x;
    gBlock[local] = global < gSortCount ? gSortEntries[global] : uint2(kLiquidInactiveKey, global);
    GroupMemoryBarrierWithGroupSync();

    const bool ascending = (global & gPassA) == 0u;
    [loop]
    for (uint j = gPassB; j > 0u; j >>= 1u)
    {
        const uint partner = local ^ j;
        // 両方が読み終わる前に片方が書くと、相手が書き換え後の値を掴んで入れ替えが壊れる。
        const uint2 mine = gBlock[local];
        const uint2 other = gBlock[partner];
        GroupMemoryBarrierWithGroupSync();
        if (partner > local && LiquidSortShouldSwap(mine, other, ascending))
        {
            gBlock[local] = other;
            gBlock[partner] = mine;
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (global < gSortCount) gSortEntries[global] = gBlock[local];
}
