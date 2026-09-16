/// @file    LiquidViscosity.cs.hlsl
/// @brief   GPU 液体: XSPH 粘性 → 年齢 → 領域外の粒を消す (刻みの締め)
/// @author  Hasegawa Jin
/// @date    2026-09-12
//
// 近傍の «速度更新後の» 値を読むので、読む状態 (t14) と書く状態 (u2) は別のバッファ。生きていない粒もそのまま写す。
#include "Bake/Fluid/LiquidNeighbors.hlsli"

StructuredBuffer<float4>   gState    : register(t14);
RWStructuredBuffer<float4> gStateOut : register(u2);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= gParticleCount) return;
    float4 positionAge = gState[2u * i];
    float4 velocityState = gState[2u * i + 1u];

    if (LiquidIsAlive(velocityState))
    {
        if (gViscosity > 0.0f)
        {
            // Σ W / ρ0 ≈ 1 なので viscosity はそのまま «近傍の速度へ寄せる割合» になる。
            float3 sum = float3(0.0f, 0.0f, 0.0f);
            [loop]
            for (uint row = 0u; row < LIQUID_NEIGHBOR_ROWS; ++row)
            {
                const uint2 range = gLiquidRanges[i * LIQUID_NEIGHBOR_ROWS + row];
                [loop]
                for (uint s = range.x; s < range.y; ++s)
                {
                    const uint j = gLiquidSorted[s].y;
                    if (j == i) continue;
                    const float3 d = positionAge.xyz - gState[2u * j].xyz;
                    const float weight = LiquidPoly6(dot(d, d)) * gInverseRest;
                    sum += (gState[2u * j + 1u].xyz - velocityState.xyz) * weight;
                }
            }
            velocityState.xyz += sum * gViscosity;
        }
        positionAge.w += gDt;
        if (any(positionAge.xyz < gBoundsMin) || any(positionAge.xyz > gBoundsMax)) velocityState.w = 2.0f;
    }
    gStateOut[2u * i] = positionAge;
    gStateOut[2u * i + 1u] = velocityState;
}
