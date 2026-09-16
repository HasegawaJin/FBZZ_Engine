/// @file    LiquidPredict.cs.hlsl
/// @brief   GPU 液体: 湧かせる → 寿命 → 重力・外力 → 位置の予測
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include "Bake/Fluid/LiquidCommon.hlsli"

StructuredBuffer<float4>   gSpawn     : register(t29); // [2i] = xyz 位置 / w 湧く時刻、[2i + 1] = xyz 初速 / w 色の鍵
RWStructuredBuffer<float4> gState     : register(u2);
RWStructuredBuffer<float4> gPredicted : register(u3); // xyz 予測位置 / w 1 = 生きている

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= gParticleCount) return;

    float4 positionAge = gState[2u * i];
    float4 velocityState = gState[2u * i + 1u];
    // CPU は刻みの頭で湧かせてから寿命を見る。湧いたばかりの粒は年齢 0 でこの刻みから重力を受ける。
    if (velocityState.w < 0.5f)
    {
        const float4 spawn = gSpawn[2u * i];
        if (spawn.w <= gTime)
        {
            positionAge = float4(spawn.xyz, 0.0f);
            velocityState = float4(gSpawn[2u * i + 1u].xyz, 1.0f);
        }
    }
    if (LiquidIsAlive(velocityState) && gLifetime > 0.0f && positionAge.w >= gLifetime)
        velocityState.w = 2.0f;

    float4 predicted = float4(positionAge.xyz, 0.0f);
    if (LiquidIsAlive(velocityState))
    {
        float3 v = velocityState.xyz;
        v.y -= gGravity * gDt;
        [loop]
        for (uint f = 0u; f < gForceCount; ++f)
            v += LiquidForceDelta(gForces[f], f, positionAge.xyz, v, gTime, gDt);
        velocityState.xyz = v;
        predicted = float4(positionAge.xyz + v * gDt, 1.0f);
    }
    gState[2u * i] = positionAge;
    gState[2u * i + 1u] = velocityState;
    gPredicted[i] = predicted;
}
