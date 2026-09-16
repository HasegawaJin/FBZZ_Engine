/// @file    FluidDivergence.cs.hlsl
/// @brief   GPU 流体: 発散 − 燃焼の膨張 (圧力解法の右辺)
/// @author  Hasegawa Jin
/// @date    2026-09-11
#include "Bake/Fluid/FluidGpuCommon.hlsli"

Texture3D<float4>   gVelocityIn : register(t0);
Texture3D<float4>   gExpansionIn : register(t1); // x 燃焼の膨張 (この刻みの Inject が書いたもの)
Texture3D<float4>   gSolidIn    : register(t2); // xyz 固体の速度 / w 1 = 固体
RWTexture3D<float4> gDivergenceOut : register(u0);

// 固体のセルは外力で速度が動いていても障害物の速度として読む (壁の «通さない» はここで効く)。
float3 V(int3 cell)
{
    const uint3 k = FluidClampCell(cell);
    const float4 solid = gSolidIn[k];
    return solid.w > 0.5f ? solid.xyz : gVelocityIn[k].xyz;
}

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id)) return;
    if (gSolidIn[id].w > 0.5f)
    {
        gDivergenceOut[id] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const int3 c = int3(id);
    const float inverseTwoH = 0.5f / gCellSize;
    float divergence = (V(c + int3(1, 0, 0)).x - V(c - int3(1, 0, 0)).x) * inverseTwoH;
    // 床は «通り抜けられない壁»。下のゴーストは鏡映 (-vy) で、床へ向かう流れを発散として拾う。
    const float self = gVelocityIn[id].y;
    const float down = c.y > 0 ? V(c - int3(0, 1, 0)).y : (gFloor != 0u ? -self : self);
    divergence += (V(c + int3(0, 1, 0)).y - down) * inverseTwoH;
    divergence += (V(c + int3(0, 0, 1)).z - V(c - int3(0, 0, 1)).z) * inverseTwoH;
    gDivergenceOut[id] = float4(divergence - gExpansionIn[id].x, 0.0f, 0.0f, 0.0f);
}
