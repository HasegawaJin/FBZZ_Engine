// FBZZ Engine
// Material/Effects/ParticleSelfShadowDensity.hlsl | VS + PS
// パーティクルの自己影用: 光源から見た密度を専用 RT へ加算する
// PSO: SOLID_NOCULL + ADDITIVE + DEPTH_NONE
//
// b0 (CameraConstants) には「光源の view / viewProjection」が入っている。
// WHY: 頂点展開のロジックを Particle.hlsl と共有したいので、行列だけを差し替える。
//      こうするとビルボードが自動的に光源へ正対し、シャドウマップと同じ扱いになる。
//
// 出力: RGB = (α, α × 光源側深度, 0)、A = 1
//   ADDITIVE の SrcBlend は SRC_ALPHA なので、A=1 にすることで RGB がそのまま積算される
//   (Debug/ParticleOverdraw.hlsl と同じ理由)。

// ParticleCommon.hlsli を最初に include する (b11 の cbuffer / ParticleVSIn /
// ParticlePSIn / ParticleBillboardVS を供給し、b2 を材質へ空ける)。
#include "Rendering/ParticleCommon.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(gParticleTex, TEX_ALBEDO_SLOT);
SamplerState gSampler     : register(SAMPLER_DEFAULT);

struct DensityPSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
    float2 localUv    : TEXCOORD1;
    float  alpha      : TEXCOORD2;
    float  lightDepth : TEXCOORD3;
};

DensityPSIn VSMain(ParticleVSIn v)
{
    // b0 に光源の行列が入っているので、共有のビルボード展開がそのまま光源へ正対する。
    // WHY 共有するか: 密度は «実際に描かれる形» を測らないと影の位置がずれる。
    //     以前はここだけ gRenderMode を見ない簡易版で、速度ストレッチした粒子の
    //     自己影だけが本体と違う形になっていた。
    ParticlePSIn billboard = ParticleBillboardVS(v);

    DensityPSIn o;
    o.svPosition = billboard.svPosition;
    o.uv         = billboard.uv;
    o.localUv    = billboard.localUv;
    o.alpha      = v.color.a;
    // 平行投影なので w=1。NDC z がそのまま [0,1] の光源側深度になる。
    o.lightDepth = o.svPosition.z;
    return o;
}

float4 PSMain(DensityPSIn p) : SV_Target0
{
    // アルファの取り出し方は本番描画と同じ規約に従う。ここがずれると
    // 黒背景素材で「全面が不透明」と誤判定し、影が板状に出る。
    float texAlpha = ResolveParticleTexel(gParticleTex.Sample(gSampler, p.uv), gEffectsFlags).a;
    float2 d = p.localUv * 2.0f - 1.0f;
    float radial = saturate(1.0f - dot(d, d));
    float alpha = texAlpha * radial * p.alpha;
    if (alpha <= 0.004f) discard;

    // R へ密度、G へ 密度×深度。後段が G/R で密度の重心 (平均深度) を得る。
    return float4(alpha, alpha * saturate(p.lightDepth), 0.0f, 1.0f);
}
