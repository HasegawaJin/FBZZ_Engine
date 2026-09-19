/// @file    FluidAdvectVelocity.cs.hlsl
/// @brief   GPU 流体: 速度の自己移流 (半ラグランジュ)
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include "Bake/Fluid/FluidGpuCommon.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX3D_T(float4, gVelocityIn, 0);
FBZZ_TEX3D_T(float4, gSolidIn, 1); // xyz 固体の速度 / w 1 = 固体
FBZZ_RWTEX3D_T(float4, gVelocityOut, 0);

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id)) return;
    // 速度は MacCormack にしない。渦の芯で振動しやすく、形の鋭さはスカラー側で十分に出る (CPU と同じ)。
    const float3 grid = float3(id) - gVelocityIn[id].xyz * (gDt / gCellSize);
    const float3 advected = gVelocityIn.SampleLevel(gFluidLinearClamp, FluidGridToUvw(grid), 0.0f).xyz;
    const float4 solid = gSolidIn[id];
    gVelocityOut[id] = float4(solid.w > 0.5f ? solid.xyz : advected, 0.0f);
}
