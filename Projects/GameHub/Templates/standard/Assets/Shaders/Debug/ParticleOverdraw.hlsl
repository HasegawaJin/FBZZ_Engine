// FBZZ Engine
// Debug/ParticleOverdraw.hlsl | Debug
// パーティクルの重なり回数 (overdraw) を可視化するための計数シェーダー
// PSO: SOLID_NOCULL + ADDITIVE + DEPTH_READ
//
// WHY: パーティクルの実コストは粒子数ではなく fill rate で決まる。
//      「budget 内なのに重い」の原因はほぼ画面上の重なりで、それは通常の絵を
//      見ていても分からない。1 フラグメントにつき一定量を加算するだけの
//      シェーダーで描き直すと、加算結果がそのまま重なり回数になる。
//
// 通常の Particle.hlsl と同じ頂点入力・同じビルボード展開を使う。
// ここがずれると「実際の描画とは違う形」を測ることになるため、
// VSMain は Particle.hlsl の実装と一致させること。

#include "Common/Binding.hlsli"
#define FBZZ_MATERIAL_CONSTANTS
#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/ParticleCommon.hlsli"

Texture2D    gParticleTex : register(TEX_ALBEDO);
SamplerState gSampler     : register(SAMPLER_DEFAULT);

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
    // GPU 経路 (ParticleGPU.hlsl) 専用。ここでは読まないが、同じ b2 を共有するため
    // ParticleRenderCB のレイアウトを 1 バイトもずらさないよう必ず宣言を揃える。
    uint  gGpuSortEnabled;
    float gSelfShadowStrength;
    // 以降は読まないが、b2 のレイアウトは GeometryPasses.hpp の ParticleRenderCB が正本。
    // 宣言を短いままにすると、後から先頭側へフィールドが増えたときに黙ってずれる。
    float gSmokeWrap;
    float gSmokeTransmission;
    float4 gTintColor;
    float gSmokeBackScatterPower;
    float gDistortionChromatic;
    float gParticlePad1;
    float gParticlePad2;
};

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

struct ParticlePSIn
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
    float2 localUv    : TEXCOORD1;
};

ParticlePSIn VSMain(ParticleVSIn v)
{
    float3 right = float3(view[0][0], view[1][0], view[2][0]);
    float3 up    = float3(view[0][1], view[1][1], view[2][1]);
    float lengthScale = 1.0f;
    if (gRenderMode == 1)
    {
        float speed = length(v.velocity);
        if (speed > 1.0e-4f)
        {
            up = v.velocity / speed;
            float3 viewDir = normalize(cameraPos - v.center);
            right = normalize(cross(up, viewDir));
            lengthScale = max(gStretchedLengthScale + speed * gStretchedVelocityScale, 0.0f);
        }
    }
    else if (gRenderMode == 2)
    {
        right = float3(1.0f, 0.0f, 0.0f);
        up = float3(0.0f, 0.0f, 1.0f);
    }
    else if (gRenderMode == 3)
    {
        up = float3(0.0f, 1.0f, 0.0f);
        float3 viewDir = normalize(cameraPos - v.center);
        right = normalize(cross(up, viewDir));
    }

    float2 corner = v.uv * 2.0f - 1.0f;
    float  s = sin(v.rotation);
    float  c = cos(v.rotation);
    corner = float2(corner.x * c - corner.y * s, corner.x * s + corner.y * c);
    float3 worldPos = v.center
                    + right * corner.x * v.size * 0.5f * gSizeAxisScaleX
                    + up    * corner.y * v.size * 0.5f * gSizeAxisScaleY * lengthScale;

    ParticlePSIn o;
    o.svPosition = mul(float4(worldPos, 1.0f), viewProjection);
    o.uv         = lerp(v.uvRect.xy, v.uvRect.zw, v.uv);
    o.localUv    = v.uv;
    return o;
}

float4 PSMain(ParticlePSIn p) : SV_Target0
{
    // 完全に透明な画素まで数えると、実際には塗っていない矩形の角まで
    // 「重なっている」ことになり過大評価になる。閾値で捨てる。
    // アルファの取り出し方は本番描画と同じ関数を使う。ここがずれると
    // 黒背景素材のとき「全面が不透明」と誤判定して overdraw を過大に見積もる。
    float alpha = ResolveParticleTexel(gParticleTex.Sample(gSampler, p.uv), gEffectsFlags).a;
    float2 d = p.localUv * 2.0f - 1.0f;
    float radial = saturate(1.0f - dot(d, d));
    if (alpha * radial <= 0.01f) discard;

    // 加算ブレンドで 1 レイヤーあたり R に 1.0 を積む。
    // 後段のカラーマップが「R の値 = 重なり枚数」として読む。
    // alpha=1 にしているのは ADDITIVE の SrcBlend が SRC_ALPHA のため
    // (これを 0 にすると何も加算されない)。
    return float4(1.0f, 0.0f, 0.0f, 1.0f);
}
