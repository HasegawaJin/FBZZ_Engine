/// @file    FluidAdvectScalar.cs.hlsl
/// @brief   GPU 流体: スカラーの移流 (gAdvectSign で前進 / 後退)
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include "Bake/Fluid/FluidGpuCommon.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX3D_T(float4, gFieldIn, 0);
FBZZ_TEX3D_T(float4, gVelocityIn, 1);
FBZZ_RWTEX3D_T(float4, gFieldOut, 0);

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id)) return;
    const float3 grid = float3(id) - gAdvectSign * gVelocityIn[id].xyz * (gDt / gCellSize);
    gFieldOut[id] = gFieldIn.SampleLevel(gFluidLinearClamp, FluidGridToUvw(grid), 0.0f);
}
