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

// ParticleCommon.hlsli を最初に include する (b11 の cbuffer / ParticleVSIn /
// ParticlePSIn / ParticleBillboardVS を供給し、b2 を材質へ空ける)。
#include "Rendering/ParticleCommon.hlsli"
#include "Common/Space.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(gParticleTex, TEX_ALBEDO_SLOT);
SamplerState gSampler     : register(SAMPLER_DEFAULT);

// 実際の描画とまったく同じ形を測るため、ビルボード展開は共有実装をそのまま使う。
ParticlePSIn VSMain(ParticleVSIn v) { return ParticleBillboardVS(v); }

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
