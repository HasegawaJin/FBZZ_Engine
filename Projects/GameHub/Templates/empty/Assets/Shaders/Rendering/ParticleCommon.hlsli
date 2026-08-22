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

#include "Common/Color.hlsli"
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
// albedo テクスチャが sRGB でエンコードされている (.meta の srgb)。
// WHY: このエンジンは _SRGB フォーマットの SRV を作らず、「シェーダーが自分で
//      SRGBToLinear する」規約で統一されている (PBR.hlsl / GBuffer.hlsl / Terrain 等)。
//      パーティクルだけこれを踏んでおらず、sRGB 値をリニアとして HDR バッファへ
//      書いた上に ACES を通していたため、彩度の高い橙が淡い黄色に飛んでいた。
//      素材はすべて sRGB とは限らない (ProceduralVFXTextures はリニアで焼く) ので
//      フラグで切り替える。
#define FBZZ_PFX_SRGB_TEXTURE   64u
// 歪み専用ノーマルマップ (t1) がバインドされている。無い場合は albedo の RG を使う。
#define FBZZ_PFX_DISTORTION_MAP 128u

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

// アルファ解決に加えて RGB をリニアへ揃える。描画に使う色は必ずこちらを通すこと。
// NOTE: アルファ抽出は「エンコードされたまま」の値で行う。輝度をアルファに使う素材は
//       見た目の明るさ (= sRGB 値) を基準に作られているため、先にリニア化すると
//       中間調のアルファだけが落ちて抜けが変わってしまう。
float4 ResolveParticleAlbedo(float4 texel, uint effectsFlags)
{
    float4 resolved = ResolveParticleTexel(texel, effectsFlags);
    if ((effectsFlags & FBZZ_PFX_SRGB_TEXTURE) != 0u) resolved.rgb = SRGBToLinear(resolved.rgb);
    return resolved;
}

// ── 煙の散乱 ──
// 巻き込み拡散: 光を透かす媒質では明暗の境界が N·L=0 で切れない。
// WHY: 素の saturate(N·L) は不透明な球の陰影で、煙に使うと陰側が硬く真っ黒に落ちる。
float ParticleWrappedDiffuse(float ndotl, float wrap)
{
    return saturate((ndotl + wrap) / (1.0f + wrap));
}

// 前方散乱による逆光透過。視線と光の向きが揃うほど強い。
// WHY: Mie 散乱の位相関数は前方に鋭く尖る。煙が「向こう側の光で縁から光る」のはこれで、
//      この項が無いと炎が煙の背後にあっても煙は暗いままになる。
float ParticleBackScatter(float3 viewDir, float3 lightDirection, float power, float strength)
{
    if (strength <= 0.0f) return 0.0f;
    return pow(saturate(dot(-viewDir, lightDirection)), max(power, 0.1f)) * strength;
}

#endif // FBZZ_PARTICLE_COMMON_INCLUDED
