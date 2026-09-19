/// @file    ParticleReactive.hlsli
/// @brief   TAA の反応マスク — 粒子が画素を覆う割合を R へ積む (CPU / GPU 経路で共有する PS)
/// @author  Hasegawa Jin
/// @date    2026-09-12
//
// WHY 要るか: 粒子は速度を書かないので、TAA は «背景の動き» で履歴を引いてくる。動く煙や火の粉は
//     背景と一緒に流れた前フレームの自分と混ざり、尾を引く (ゴースト)。粒子が覆う画素だけ履歴を
//     信じる割合を下げさせる (UE の Responsive AA / FSR の reactive mask と同じ考え)。
// 前提: Material/Effects/ParticleMaterial.hlsli を先に include する (gParticleTex / gSampler / VSMain)。
#ifndef PARTICLE_REACTIVE_HLSLI
#define PARTICLE_REACTIVE_HLSLI
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(gReactiveMotionVectors, 6);
FBZZ_TEX2D(gReactiveSceneDepth, TEX_DEPTH_SLOT);

float4 PSMain(ParticlePSIn p) : SV_Target0
{
    // 不透明物の後ろにある粒子は画面に出ていない。そこで履歴を捨てると輪郭がちらつくだけになる。
    const float sceneDepth = gReactiveSceneDepth.Load(int3(int2(p.svPosition.xy), 0)).r;
    if (p.svPosition.z > sceneDepth) discard;

    float coverage = p.color.a * gTintColor.a * RadialMask(p.localUv);
    if ((gEffectsFlags & FBZZ_PFX_VOLUMETRIC) == 0u)
    {
        float2 currentUv;
        const float4 tex = SampleParticleFlipbook(gParticleTex, gReactiveMotionVectors, gSampler,
                                                  p.uv, p.nextUv, p.spriteBlend, gEffectsFlags, currentUv);
        // 加算の素材は黒背景でアルファが全面 1 のことが多い。見えている明るさを覆いとみなす。
        coverage *= (gEffectsFlags & FBZZ_PFX_ADDITIVE) != 0u
            ? saturate(dot(tex.rgb * p.color.rgb * gTintColor.rgb * gEmissiveScale, float3(0.2126f, 0.7152f, 0.0722f)))
            : tex.a;
    }
    return float4(saturate(coverage), 0.0f, 0.0f, 1.0f);
}

#endif // PARTICLE_REACTIVE_HLSLI
