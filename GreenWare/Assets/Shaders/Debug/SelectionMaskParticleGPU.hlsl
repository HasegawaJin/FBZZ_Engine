// FBZZ Engine
// Debug/SelectionMaskParticleGPU.hlsl | fbzz::renderer
// GPUパーティクルの可視ピクセルをSelection Outline用マスクへ合成する

// 構造体・StructuredBuffer・定数バッファ・ビルボード VS は契約ヘッダーが供給する。
// ParticleGPU.hlsl とまったく同じ展開を使うことで、輪郭が本体からずれない。
#define FBZZ_PARTICLE_GPU
#include "Material/Effects/ParticleMaterial.hlsli"
#include "Common/Space.hlsli"


float4 PSMain(ParticlePSIn input) : SV_Target0
{
    float alpha = RadialMask(input.localUv) * input.color.a;
    if ((gEffectsFlags & FBZZ_PFX_VOLUMETRIC) == 0u)
        alpha *= ResolveParticleTexel(
            gParticleTex.Sample(gSampler, input.uv), gEffectsFlags).a;
    if (alpha < 0.02f) discard;
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
