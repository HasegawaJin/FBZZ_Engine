/// @file    LiquidDelta.cs.hlsl
/// @brief   GPU 液体: 密度拘束の補正 (s_corr 込み) → 床と障害物の押し出し (FluidLiquidSolver::SolveDensity の後半)
/// @author  Hasegawa Jin
/// @date    2026-09-12
//
// 読む予測位置 (t14) と書く予測位置 (u2) は別のバッファ。全員の補正を今の位置から求めてから動かす (CPU と同じ)。
#include "Bake/Fluid/LiquidNeighbors.hlsli"

StructuredBuffer<float4>   gPredicted    : register(t14);
StructuredBuffer<float>    gLambda       : register(t30);
RWStructuredBuffer<float4> gPredictedOut : register(u2);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= gParticleCount) return;
    const float4 pi = gPredicted[i];
    if (pi.w < 0.5f)
    {
        gPredictedOut[i] = pi;
        return;
    }

    const float lambdaI = gLambda[i];
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
            const float3 d = pi.xyz - gPredicted[j].xyz;
            const float r2 = dot(d, d);
            const float scale = LiquidSpikyScale(sqrt(r2));
            if (scale == 0.0f) continue;
            const float ratio = LiquidPoly6(r2) / gTensileReference;
            const float ratio2 = ratio * ratio;
            const float tensile = -gTensileScale * ratio2 * ratio2;
            sum += d * ((lambdaI + gLambda[j] + tensile) * scale * gInverseRest);
        }
    }
    // 1 反復の補正を粒子半径の半分までに抑える。発生直後の重なりで弾け飛ばないように。
    const float len = length(sum);
    if (len > gMaxCorrection) sum *= gMaxCorrection / len;

    float3 p = pi.xyz + sum;
    if (gFloorEnabled != 0u && p.y < gFloorHeight + gRadius) p.y = gFloorHeight + gRadius;
    // 障害物の表面から粒子半径ぶん外へ出す。反復ごとに行うので、最後の反復の後は必ず外に居る。
    [loop]
    for (uint c = 0u; c < gColliderCount; ++c)
    {
        const LiquidCollider collider = gColliders[c];
        if (collider.sizeActive.w < 0.5f) continue;
        const float distance = LiquidColliderDistance(collider, p);
        if (distance >= gRadius) continue;
        p += LiquidColliderNormal(collider, p) * (gRadius - distance);
    }
    gPredictedOut[i] = float4(p, 1.0f);
}
