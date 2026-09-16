/// @file    LiquidSortKeys.cs.hlsl
/// @brief   GPU 液体: 粒子の (セル番号, 粒子番号) を並べ替え用に書く
/// @author  Hasegawa Jin
/// @date    2026-09-12
//
// dispatch: gSortCount / LIQUID_SORT_BLOCK。gPassA = 位置の間隔 (1 = 予測位置 float4 × 1 / 2 = 状態 float4 × 2)。
#include "Bake/Fluid/LiquidSort.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_SBUFFER_T(float4, gPositions, 14);

[numthreads(LIQUID_SORT_BLOCK, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= gSortCount) return;

    // 生きていない粒と、2 のべき乗へ切り上げた詰め物は最大の鍵で末尾へ落とす。
    uint key = kLiquidInactiveKey;
    if (i < gParticleCount)
    {
        const float4 position = gPositions[i * gPassA];
        bool alive = false;
        if (gPassA == 1u) alive = position.w > 0.5f;
        else alive = LiquidIsAlive(gPositions[i * gPassA + 1u]);
        if (alive) key = LiquidCellKey(LiquidCellCoord(position.xyz));
    }
    gSortEntries[i] = uint2(key, i);
}
