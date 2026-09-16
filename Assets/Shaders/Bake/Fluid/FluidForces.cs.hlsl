/// @file    FluidForces.cs.hlsl
/// @brief   GPU 流体: 浮力・風・乱流・減衰 (FluidGasSolver::ApplyForces)
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include "Bake/Fluid/FluidGpuCommon.hlsli"

Texture3D<float4>   gVelocityIn  : register(t0);
Texture3D<float4>   gScalarsIn   : register(t1);
RWTexture3D<float4> gVelocityOut : register(u0);

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id)) return;
    float3 v = gVelocityIn[id].xyz;
    const float4 s = gScalarsIn[id];
    const float3 p = FluidCellPosition(id);
    v.y += (gBuoyancy * s.y - gWeight * s.x) * gDt;
    v += gWind * gDt;
    if (gTurbulence != 0.0f)
    {
        // 時間でゆっくりずらして «止まった渦» にしない。
        const float3 curl = FluidCurlNoise(float3(p.x * gTurbulenceScale + gNoiseOffset.x,
                                                  p.y * gTurbulenceScale + gNoiseOffset.y - gTime * 0.35f,
                                                  p.z * gTurbulenceScale + gNoiseOffset.z + gTime * 0.2f));
        v += curl * gTurbulence * gDt;
    }
    // 部品の力は減衰より前 (CPU の ApplyForces と同じ順)。Drag は減衰前の速度を見る。
    [loop] for (uint i = 0; i < gForceCount; ++i)
        v += FluidForceDelta(gForces[i], i, p, v, gTime, gDt);
    gVelocityOut[id] = float4(v * gVelocityKeep, 0.0f);
}
