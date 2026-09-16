/// @file    FluidProject.cs.hlsl
/// @brief   GPU 流体: 圧力勾配を引いて発散を消す
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include "Bake/Fluid/FluidGpuCommon.hlsli"

Texture3D<float4>   gVelocityIn  : register(t0);
Texture3D<float4>   gPressureIn  : register(t1);
Texture3D<float4>   gSolidIn     : register(t2); // xyz 固体の速度 / w 1 = 固体
RWTexture3D<float4> gVelocityOut : register(u0);

// NOTE: 出口を 1 つにしてある。早期 return は FXC が X4000 で咎める。
// 障害物のセルは Jacobi と同じく ∂p/∂n = 0 (自分の圧力を映す)。
float PressureAt(int3 cell, float self, bool solid)
{
    float pressure = solid ? self : 0.0f;
    if (FluidInside(cell))
    {
        const uint3 k = uint3(cell);
        pressure = gSolidIn[k].w > 0.5f ? self : gPressureIn[k].x;
    }
    return pressure;
}

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id)) return;
    const float4 solid = gSolidIn[id];
    if (solid.w > 0.5f)
    {
        gVelocityOut[id] = float4(solid.xyz, 0.0f);
        return;
    }
    const int3 c = int3(id);
    const float self = gPressureIn[id].x;
    const float inverseTwoH = 0.5f / gCellSize;
    float3 v = gVelocityIn[id].xyz;
    v.x -= (PressureAt(c + int3(1, 0, 0), self, false) - PressureAt(c - int3(1, 0, 0), self, false)) * inverseTwoH;
    v.y -= (PressureAt(c + int3(0, 1, 0), self, false) - PressureAt(c - int3(0, 1, 0), self, gFloor != 0u)) * inverseTwoH;
    v.z -= (PressureAt(c + int3(0, 0, 1), self, false) - PressureAt(c - int3(0, 0, 1), self, false)) * inverseTwoH;
    if (gFloor != 0u && id.y == 0u) v.y = max(v.y, 0.0f);
    gVelocityOut[id] = float4(v, 0.0f);
}
