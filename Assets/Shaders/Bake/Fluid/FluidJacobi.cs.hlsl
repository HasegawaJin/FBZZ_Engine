/// @file    FluidJacobi.cs.hlsl
/// @brief   GPU 流体: 圧力の Jacobi 反復 1 回
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include "Bake/Fluid/FluidGpuCommon.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX3D_T(float4, gPressureIn, 0);
FBZZ_TEX3D_T(float4, gDivergenceIn, 1);
FBZZ_TEX3D_T(float4, gSolidIn, 2); // w 1 = 固体
FBZZ_RWTEX3D_T(float4, gPressureOut, 0);

// 境界: 開いた縁は p = 0 (流れが自由に出ていく)、床と障害物は ∂p/∂n = 0 (通り抜けない)。
// ∂p/∂n = 0 は «隣を自分と同じ圧力とみなす» こと。和にも数にも入れなければ同じ解になり、収束も速い。
void Neighbor(int3 cell, bool solid, inout float sum, inout float count)
{
    if (FluidInside(cell))
    {
        const uint3 k = uint3(cell);
        if (gSolidIn[k].w < 0.5f)
        {
            sum += gPressureIn[k].x;
            count += 1.0f;
        }
    }
    else if (!solid)
    {
        count += 1.0f;
    }
}

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id)) return;
    if (gSolidIn[id].w > 0.5f)
    {
        gPressureOut[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const int3 c = int3(id);
    float sum = 0.0f;
    float count = 0.0f;
    Neighbor(c - int3(1, 0, 0), false, sum, count);
    Neighbor(c + int3(1, 0, 0), false, sum, count);
    Neighbor(c - int3(0, 1, 0), gFloor != 0u, sum, count);
    Neighbor(c + int3(0, 1, 0), false, sum, count);
    Neighbor(c - int3(0, 0, 1), false, sum, count);
    Neighbor(c + int3(0, 0, 1), false, sum, count);
    const float h2 = gCellSize * gCellSize;
    const float pressure = count > 0.0f ? (sum - h2 * gDivergenceIn[id].x) / count : 0.0f;
    gPressureOut[id] = float4(pressure, 0.0f, 0.0f, 0.0f);
}
