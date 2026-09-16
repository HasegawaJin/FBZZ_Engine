/// @file    FluidSolid.cs.hlsl
/// @brief   GPU 流体: 障害物の中のセルを固体として書き出す (刻みの頭に 1 回)
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include "Bake/Fluid/FluidGpuCommon.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_RWTEX3D_T(float4, gSolidOut, 0); // xyz 固体の速度 / w 1 = 固体, 0 = 流体

[numthreads(4, 4, 4)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id)) return;
    const float3 p = FluidCellPosition(id);
    float4 solid = float4(0.0f, 0.0f, 0.0f, 0.0f);
    // 重なった障害物はリストの先のものが勝つ。速度を混ぜると、どちらでもない向きに押してしまう。
    [loop] for (uint i = 0; i < gColliderCount; ++i)
    {
        const FluidGpuCollider collider = gColliders[i];
        if (solid.w < 0.5f && collider.sizeActive.w > 0.5f && FluidColliderDistance(collider, p) < 0.0f)
            solid = float4(collider.velocity.xyz, 1.0f);
    }
    gSolidOut[id] = solid;
}
