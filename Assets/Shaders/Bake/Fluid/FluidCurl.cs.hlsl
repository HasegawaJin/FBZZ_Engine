/// @file    FluidCurl.cs.hlsl
/// @brief   GPU 流体: 渦度 (curl) とその大きさ
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include "Bake/Fluid/FluidGpuCommon.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX3D_T(float4, gVelocityIn, 0);
FBZZ_RWTEX3D_T(float4, gCurlOut, 0); // xyz 渦度 / w 大きさ

// 軸方向の中心差分 (縁は片側差分)。FluidGasSolver::Derivative と同じ。
float3 VelocityDerivative(Texture3D<float4> field, uint3 id, uint axis)
{
    int3 lo = int3(id);
    int3 hi = int3(id);
    lo[axis] = max(int(id[axis]) - 1, 0);
    hi[axis] = min(int(id[axis]) + 1, int(gResolution) - 1);
    const float span = float(hi[axis] - lo[axis]) * gCellSize;
    return span > 0.0f ? (field[uint3(hi)].xyz - field[uint3(lo)].xyz) / span : 0.0f;
}

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id)) return;
    const float3 alongX = VelocityDerivative(gVelocityIn, id, 0);
    const float3 alongY = VelocityDerivative(gVelocityIn, id, 1);
    const float3 alongZ = VelocityDerivative(gVelocityIn, id, 2);
    const float3 w = float3(alongY.z - alongZ.y, alongZ.x - alongX.z, alongX.y - alongY.x);
    gCurlOut[id] = float4(w, length(w));
}
