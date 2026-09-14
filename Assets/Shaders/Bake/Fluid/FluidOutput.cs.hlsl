/// @file    FluidOutput.cs.hlsl
/// @brief   GPU 流体: 場を Volume Flipbook Baker のボリュームへ書く
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include "Bake/Fluid/FluidGpuCommon.hlsli"

Texture3D<float4>   gScalarsIn   : register(t0);
Texture3D<float4>   gVelocityIn  : register(t1);
RWTexture3D<float4> gMediumOut   : register(u0); // R 密度 / G 温度 / B colorKey / A 液体の割合 (VolumeFill と同じ)
RWTexture3D<float4> gVelocityOut : register(u1);

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id)) return;
    const float4 s = gScalarsIn[id];
    gMediumOut[id] = float4(max(s.x, 0.0f) * gDensityScale, max(s.y, 0.0f) * gTemperatureScale, FluidColorKey(s), 0.0f);
    gVelocityOut[id] = float4(gVelocityIn[id].xyz, 0.0f);
}
