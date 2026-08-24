// FBZZ Engine
// Debug/SelectionMaskParticle.hlsl | fbzz::renderer
// CPUパーティクルの可視ピクセルをSelection Outline用マスクへ合成する

// ParticleCommon.hlsli を最初に include する (b11 の cbuffer / ParticleVSIn /
// ParticlePSIn / ParticleBillboardVS を供給し、b2 を材質へ空ける)。
#include "Rendering/ParticleCommon.hlsli"
#include "Common/Space.hlsli"

Texture2D    gParticleTex : register(TEX_ALBEDO);
SamplerState gSampler     : register(SAMPLER_DEFAULT);

// 通常描画とMask描画のビルボード形状が1pxでもずれると輪郭が本体から浮いて見えるため、
// 展開は共有実装をそのまま使う。
ParticlePSIn VSMain(ParticleVSIn v) { return ParticleBillboardVS(v); }

// 矩形クワッドではなく、実際に見えているテクセルだけをMaskへ書く。
float4 PSMain(ParticlePSIn input) : SV_Target0
{
    const float radial = RadialMask(input.localUv);
    float alpha = radial * input.color.a;
    if ((gEffectsFlags & FBZZ_PFX_VOLUMETRIC) == 0u)
    {
        const float4 current = ResolveParticleTexel(
            gParticleTex.Sample(gSampler, input.uv), gEffectsFlags);
        const float4 next = ResolveParticleTexel(
            gParticleTex.Sample(gSampler, input.nextUv), gEffectsFlags);
        alpha *= lerp(current.a, next.a, saturate(input.spriteBlend));
    }
    if (alpha < 0.02f) discard;
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}
