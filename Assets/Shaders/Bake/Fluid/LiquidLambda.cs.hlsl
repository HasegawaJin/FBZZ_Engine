/// @file    LiquidLambda.cs.hlsl
/// @brief   GPU 液体: 密度拘束の λ (FluidLiquidSolver::SolveDensity の前半)
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include "Bake/Fluid/LiquidNeighbors.hlsli"

StructuredBuffer<float4>  gPredicted : register(t14);
RWStructuredBuffer<float> gLambdaOut : register(u2);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint i = id.x;
    if (i >= gParticleCount) return;
    const float4 pi = gPredicted[i];
    if (pi.w < 0.5f)
    {
        gLambdaOut[i] = 0.0f;
        return;
    }

    float density = LiquidPoly6(0.0f);
    float3 gradient = float3(0.0f, 0.0f, 0.0f);
    float gradientSquared = 0.0f;
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
            if (r2 >= gH2) continue;
            density += LiquidPoly6(r2);
            const float3 g = d * (LiquidSpikyScale(sqrt(r2)) * gInverseRest);
            gradient += g;
            gradientSquared += dot(g, g);
        }
    }
    float constraint = density * gInverseRest - 1.0f;
    // 負 (まばら) の側は «引き寄せ» になる。cohesion で効きを絞る (CPU と同じ)。
    if (constraint < 0.0f) constraint *= gCohesion;
    gLambdaOut[i] = -constraint / (dot(gradient, gradient) + gradientSquared + gRelaxation);
}
