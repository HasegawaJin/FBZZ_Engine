// FBZZ Engine
// Rendering/ParticleCommon.hlsli
// パーティクル描画シェーダーが共有する effectsFlags の定義とアルファ解決
//
// WHY: effectsFlags のビットは Particle.hlsl / ParticleGPU.hlsl / ParticleOverdraw.hlsl の
//      3 ファイルで生の数値 (1u, 2u, 4u...) として使われていた。ビットを 1 つ足すたびに
//      3 箇所を見比べる必要があり、片方だけ直すと「CPU では効くが GPU では効かない」
//      形で静かに壊れる。定義と解釈をここへ集約する。
//      C++ 側は ParticlePass.cpp の cb.effectsFlags 組み立てと一致させること。

#ifndef FBZZ_PARTICLE_COMMON_INCLUDED
#define FBZZ_PARTICLE_COMMON_INCLUDED

#include "Rendering/Mask.hlsli"

// ── effectsFlags のビット割り当て ──
#define FBZZ_PFX_DISTORTION     1u   // 背景を屈折させる (熱歪み)
#define FBZZ_PFX_SIX_WAY        2u   // 疑似法線による lit smoke
#define FBZZ_PFX_MOTION_VECTOR  4u   // フリップブックの motion vector ブレンド
#define FBZZ_PFX_RECEIVE_SHADOW 8u   // シャドウマップを受ける
#define FBZZ_PFX_VOLUMETRIC     16u  // ビルボード内レイマーチ
// 事前乗算アルファで描かれている。RGB が既に alpha 込みの値であることを示す。
// WHY: SrcBlend=ONE のため、ソフトパーティクルの fade を alpha だけに掛けると
//      RGB が減らず白い縁が残る。PS 側で RGB にも同じ係数を掛けるために要る。
#define FBZZ_PFX_PREMULTIPLIED  32u

// アルファの取り出し方は effectsFlags の bit8-10 (3 ビット) に格納する。
// 値は Rendering/Mask.hlsli の FBZZ_MASK_* をそのまま使う。
// WHY: パーティクルだけ別の番号体系にすると、同じ「黒背景素材」の指定が
//      マテリアルとパーティクルで違う値になり、両方を触る人が必ず取り違える。
//      チャンネル選択の語彙は全マテリアルで 1 つに揃える。
#define FBZZ_PALPHA_SHIFT 8u
#define FBZZ_PALPHA_MASK  7u

// パーティクル素材からアルファを取り出す。RGB は色としてそのまま残す。
// (輝度をアルファにする素材でも、RGB は炎や煙の色として意味を持つため)
float4 ResolveParticleTexel(float4 texel, uint effectsFlags)
{
    const uint channel = (effectsFlags >> FBZZ_PALPHA_SHIFT) & FBZZ_PALPHA_MASK;
    // 通常のアルファ素材は無加工で返す (saturate も掛けず、既存の見た目を厳密に保つ)。
    if (channel == FBZZ_MASK_ALPHA) return texel;
    return ApplyMaskToAlpha(texel, channel);
}

#endif // FBZZ_PARTICLE_COMMON_INCLUDED
