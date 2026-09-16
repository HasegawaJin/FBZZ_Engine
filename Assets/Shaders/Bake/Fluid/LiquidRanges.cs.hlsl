/// @file    LiquidRanges.cs.hlsl
/// @brief   GPU 液体: 粒子ごとに近傍 27 セルを 9 本の連続した範囲として引いておく
/// @author  Hasegawa Jin
/// @date    2026-09-12
//
// 近傍は反復の前に 1 回だけ組む (CPU の BuildNeighbors と同じ)。反復・粘性はこの範囲を使い回す。
#include "Bake/Fluid/LiquidGrid.hlsli"

StructuredBuffer<float4>  gPredicted : register(t14);
RWStructuredBuffer<uint2> gRangesOut : register(u2);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= gParticleCount) return;

    const float4 p = gPredicted[i];
    const bool alive = p.w > 0.5f;
    const int3 cell = LiquidCellCoord(p.xyz);
    const int xLo = max(cell.x - 1, 0);
    const int xHi = min(cell.x + 1, int(gCellsX) - 1);
    uint row = 0u;
    [loop]
    for (int oz = -1; oz <= 1; ++oz)
    {
        [loop]
        for (int oy = -1; oy <= 1; ++oy)
        {
            const int y = cell.y + oy;
            const int z = cell.z + oz;
            uint2 range = uint2(0u, 0u);
            if (alive && y >= 0 && y < int(gCellsY) && z >= 0 && z < int(gCellsZ))
                range = LiquidRowRange(xLo, xHi, y, z);
            gRangesOut[i * 9u + row] = range;
            ++row;
        }
    }
}
