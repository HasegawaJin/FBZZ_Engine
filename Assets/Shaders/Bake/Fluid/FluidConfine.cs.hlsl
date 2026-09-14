/// @file    FluidConfine.cs.hlsl
/// @brief   GPU 流体: 渦度保存 (Fedkiw et al. 2001)
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include "Bake/Fluid/FluidGpuCommon.hlsli"

Texture3D<float4>   gVelocityIn  : register(t0);
Texture3D<float4>   gCurlIn      : register(t1);
RWTexture3D<float4> gVelocityOut : register(u0);

float CurlLengthDerivative(uint3 id, uint axis)
{
    int3 lo = int3(id);
    int3 hi = int3(id);
    lo[axis] = max(int(id[axis]) - 1, 0);
    hi[axis] = min(int(id[axis]) + 1, int(gResolution) - 1);
    const float span = float(hi[axis] - lo[axis]) * gCellSize;
    return span > 0.0f ? (gCurlIn[uint3(hi)].w - gCurlIn[uint3(lo)].w) / span : 0.0f;
}

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id)) return;
    float3 v = gVelocityIn[id].xyz;
    const float3 eta = float3(CurlLengthDerivative(id, 0), CurlLengthDerivative(id, 1), CurlLengthDerivative(id, 2));
    const float len = length(eta);
    if (len >= 1.0e-6f)
    {
        // f = ε h (N × ω)。数値拡散で消える細かい渦を、渦度の強い方へ押し戻す。
        const float3 n = eta / len;
        v += cross(n, gCurlIn[id].xyz) * (gVorticity * gCellSize * gDt);
    }
    gVelocityOut[id] = float4(v, 0.0f);
}
