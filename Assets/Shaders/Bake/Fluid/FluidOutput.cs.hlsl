/// @file    FluidOutput.cs.hlsl
/// @brief   GPU 流体: 場を Volume Flipbook Baker のボリュームへ書く
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include "Bake/Fluid/FluidGpuCommon.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX3D_T(float4, gScalarsIn, 0);
FBZZ_TEX3D_T(float4, gVelocityIn, 1);
FBZZ_RWTEX3D_T(float4, gMediumOut, 0); // R 密度 / G 温度 / B colorKey / A 液体の割合 (VolumeFill と同じ)
FBZZ_RWTEX3D_T(float4, gVelocityOut, 1);

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id)) return;
    const float4 s = gScalarsIn[id];
    gMediumOut[id] = float4(max(s.x, 0.0f) * gDensityScale, max(s.y, 0.0f) * gTemperatureScale, FluidColorKey(s), 0.0f);
    gVelocityOut[id] = float4(gVelocityIn[id].xyz, 0.0f);
}
