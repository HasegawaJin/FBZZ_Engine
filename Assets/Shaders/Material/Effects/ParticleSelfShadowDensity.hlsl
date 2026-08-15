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

#include "Common/Binding.hlsli"
#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/ParticleCommon.hlsli"

Texture2D    gParticleTex : register(TEX_ALBEDO);
SamplerState gSampler     : register(SAMPLER_DEFAULT);

// LAYOUT: GeometryPasses.hpp の ParticleRenderCB が正本。読まない項目も宣言を落とさない。
cbuffer ParticleRenderConstants : register(CB_MATERIAL)
{
    uint  gRenderMode;
    float gStretchedVelocityScale;
    float gStretchedLengthScale;
    float gSoftParticleFadeDistance;
    uint  gSoftParticles;
    uint  gMaxParticles;
    uint  gEffectsFlags;
    float gDistortionStrength;
    float gLightingStrength;
    float gEmissiveScale;
    float gMotionVectorStrength;
    float gScreenWidth;
    float gScreenHeight;
    float gSizeAxisScaleX;
    float gSizeAxisScaleY;
    float gShadowStrength;
    uint  gVolumetricSteps;
    float gVolumetricDensity;
    float gVolumetricAnisotropy;
    float gVolumetricNoiseScale;
    uint  gGpuSortEnabled;
    float gSelfShadowStrength;
    float gParticlePad1;
    float gParticlePad2;
};

// LAYOUT: Particle.hlsl の ParticleVSIn と一致させること (同じ頂点バッファを読む)。
struct ParticleVSIn
{
    float3 center : POSITION;
    float2 uv     : TEXCOORD0;
    float4 color  : COLOR;
    float  size   : TEXCOORD1;
    float  rotation : TEXCOORD2;
    float4 uvRect   : TEXCOORD3;
    float3 velocity : TEXCOORD4;
    float4 nextUvRect : TEXCOORD5;
    float spriteBlend : TEXCOORD6;
};

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
    // 光源の view 行列の列 0/1 = 光源から見た右/上。ビルボードが光源へ正対する。
    float3 right = float3(view[0][0], view[1][0], view[2][0]);
    float3 up    = float3(view[0][1], view[1][1], view[2][1]);

    float2 corner = v.uv * 2.0f - 1.0f;
    float  s = sin(v.rotation);
    float  c = cos(v.rotation);
    corner = float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);
    float3 worldPos = v.center
                    + right * corner.x * v.size * 0.5f * gSizeAxisScaleX
                    + up    * corner.y * v.size * 0.5f * gSizeAxisScaleY;

    DensityPSIn o;
    o.svPosition = mul(float4(worldPos, 1.0f), viewProjection);
    o.uv         = lerp(v.uvRect.xy, v.uvRect.zw, v.uv);
    o.localUv    = v.uv;
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
