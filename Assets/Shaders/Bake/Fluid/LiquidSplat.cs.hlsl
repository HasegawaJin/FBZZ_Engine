/// @file    LiquidSplat.cs.hlsl
/// @brief   GPU 液体: 粒子を焼き用ボリュームへ塗る (PackLiquidVolume と同じ意味)
/// @author  Hasegawa Jin
/// @date    2026-09-12
//
// WHY 粒子から撒かず、ボクセルから近くの粒子を集めるか: 撒くと 1 ボクセルへ複数の粒子が同時に足し込み、
//     float の原子加算が要る (DX11 には無い)。集めればボクセルごとに 1 スレッドで閉じる。
// 媒質: R = 粒子の山の和 / G = 0 / B = 色の鍵 (山の重みで平均) / A = 液体なら 1。速度: 山の重みで平均。
#include "Bake/Fluid/LiquidGrid.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_SBUFFER_T(float4, gState, 14);
FBZZ_SBUFFER_T(float4, gSpawn, 29); // [2i + 1].w = 色の鍵
FBZZ_RWTEX3D_T(float4, gMediumOut, 0);
FBZZ_RWTEX3D_T(float4, gVelocityOut, 1);

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= gResolution)) return;

    // bake 空間 [-1,1] の 1 辺を gResolution セルに割る。セル中心は (i + 0.5) / cellsPerUnit − 1。
    const float3 voxel = float3(id);
    const float3 center = (voxel + 0.5f) / gCellsPerUnit - 1.0f;
    // 塗る半径は寿命で細るが、下限 (0.75 セル) より大きくはならない。最大の半径の中の粒だけを見ればよい。
    const float reach = max(gSplatRadius, kLiquidMinSplatCells / gCellsPerUnit);
    const int3 lo = LiquidCellCoord(center - reach);
    const int3 hi = LiquidCellCoord(center + reach);

    float amountSum = 0.0f;
    float keySum = 0.0f;
    float3 velocitySum = float3(0.0f, 0.0f, 0.0f);
    [loop]
    for (int z = lo.z; z <= hi.z; ++z)
    {
        [loop]
        for (int y = lo.y; y <= hi.y; ++y)
        {
            const uint2 range = LiquidRowRange(lo.x, hi.x, y, z);
            [loop]
            for (uint s = range.x; s < range.y; ++s)
            {
                const uint j = gLiquidSorted[s].y;
                const float4 positionAge = gState[2u * j];
                const float4 velocityState = gState[2u * j + 1u];
                if (!LiquidIsAlive(velocityState)) continue;
                // 寿命がある飛沫は細りながら消える (いきなり消すとコマ間でポツポツ抜けて見える)。
                const float life = gLifetime > 0.0f ? saturate(1.0f - positionAge.w / gLifetime) : 1.0f;
                if (life <= 0.0f) continue;
                // 低い解像度でも液体が消えないよう半径を 0.75 セルまで広げ、広げた分だけ量を減らして体積を保つ。
                const float trueRadius = gSplatRadius * sqrt(life) * gCellsPerUnit;
                const float r = max(trueRadius, kLiquidMinSplatCells);
                const float shrink = trueRadius / r;
                const float3 cell = (positionAge.xyz + 1.0f) * gCellsPerUnit - 0.5f;
                const float3 q = (voxel - cell) / r;
                const float q2 = dot(q, q);
                if (q2 >= 1.0f) continue;
                const float falloff = (1.0f - q2) * (1.0f - q2) * (1.0f - q2) * (shrink * shrink * shrink);
                amountSum += falloff;
                velocitySum += velocityState.xyz * falloff;
                keySum += gSpawn[2u * j + 1u].w * falloff;
            }
        }
    }

    float4 medium = float4(amountSum, 0.0f, 0.0f, 0.0f);
    float3 velocity = float3(0.0f, 0.0f, 0.0f);
    // 量を減らした粒の縁は重みが極小になる。閾値で切ると «密度はあるのに液体でないセル» ができる。
    if (amountSum > 0.0f)
    {
        const float inverse = 1.0f / amountSum;
        medium.b = keySum * inverse;
        medium.a = 1.0f;
        velocity = velocitySum * inverse;
    }
    gMediumOut[id] = medium;
    gVelocityOut[id] = float4(velocity, 0.0f);
}
