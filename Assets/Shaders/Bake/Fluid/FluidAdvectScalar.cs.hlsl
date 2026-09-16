/// @file    FluidAdvectScalar.cs.hlsl
/// @brief   GPU 流体: スカラーの移流 (gAdvectSign で前進 / 後退)
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include "Bake/Fluid/FluidGpuCommon.hlsli"

Texture3D<float4>   gFieldIn    : register(t0);
Texture3D<float4>   gVelocityIn : register(t1);
RWTexture3D<float4> gFieldOut   : register(u0);

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id)) return;
    const float3 grid = float3(id) - gAdvectSign * gVelocityIn[id].xyz * (gDt / gCellSize);
    gFieldOut[id] = gFieldIn.SampleLevel(gFluidLinearClamp, FluidGridToUvw(grid), 0.0f);
}
