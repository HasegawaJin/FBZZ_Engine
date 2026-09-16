/// @file    LiquidClear.cs.hlsl
/// @brief   GPU 液体: 全粒子を «まだ湧いていない» 状態へ戻す
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include "Bake/Fluid/LiquidCommon.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_RWSBUFFER_T(float4, gStateOut, 2);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gParticleCount * 2u) return;
    gStateOut[id.x] = float4(0.0f, 0.0f, 0.0f, 0.0f);
}
