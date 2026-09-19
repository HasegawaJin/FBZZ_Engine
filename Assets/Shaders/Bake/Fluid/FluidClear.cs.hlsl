/// @file    FluidClear.cs.hlsl
/// @brief   GPU 流体: 作ったばかりの格子を 0 で埋める
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include "Bake/Fluid/FluidGpuCommon.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_RWTEX3D_T(float4, gOut0, 0);
FBZZ_RWTEX3D_T(float4, gOut1, 1);

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id)) return;
    gOut0[id] = 0.0f;
    gOut1[id] = 0.0f;
}
