// FBZZ Engine
// Rendering/Mask.hlsli
// 全マテリアル共通のマスク処理ユーティリティ
//
// WHY: 「テクスチャのどこを不透明度 / 強度として読むか」は素材ごとに違う。
//      アルファ付き PNG、黒背景でアルファ無しのエフェクト素材、白背景の版下、
//      R/G/B へ別々の情報を詰めたパック済みマスク — どれも現場では普通に混ざる。
//      これをシェーダーごとに書くと、素材を差し替えるたびにシェーダーを直すか、
//      画像編集ソフトで素材を作り直すことになる。読み方をここへ集約し、
//      マテリアル側は「どのチャンネルをどう解釈するか」を定数で選ぶだけにする。
//
//      あわせて、カットアウトの clip・縁のフェード・コントラスト調整といった
//      各シェーダーへ散っていた定型もここへ集める。
//
// 使い方 (代表例):
//   float mask = SampleMask(tex, samp, uv, FBZZ_MASK_LUMINANCE);  // 黒背景素材
//   mask = RemapMask(mask, 0.35f, 0.65f);                         // 縁を締める
//   ClipMask(mask, alphaCutoff);                                  // カットアウト

#ifndef FBZZ_MASK_HLSLI
#define FBZZ_MASK_HLSLI

#include "Common/Color.hlsli"

// ---- チャンネル選択 -------------------------------------------------------
// 値は C++ 側の列挙と一致させること
// (パーティクルは ParticleAlphaSource、他マテリアルは各自の定数)。
#define FBZZ_MASK_ALPHA          0u  // 通常。アルファチャンネルをそのまま使う
#define FBZZ_MASK_LUMINANCE      1u  // 黒 = 透明。RGB の輝度をマスクにする
#define FBZZ_MASK_LUMINANCE_INV  2u  // 白 = 透明。輝度を反転してマスクにする
#define FBZZ_MASK_RED            3u  // R チャンネル (パック済みマスクの1枚目)
#define FBZZ_MASK_GREEN          4u  // G チャンネル
#define FBZZ_MASK_BLUE           5u  // B チャンネル
#define FBZZ_MASK_ALPHA_INV      6u  // アルファ反転

// 既にサンプル済みの texel からマスク値を取り出す。
// WHY: サンプリングと分離しておくと、フリップブックのように「複数回サンプルして
//      から合成する」経路でも、合成前の各コマへ同じ解釈を適用できる。
float ResolveMask(float4 texel, uint channel)
{
    if (channel == FBZZ_MASK_ALPHA)         return saturate(texel.a);
    if (channel == FBZZ_MASK_LUMINANCE)     return saturate(Luminance(texel.rgb));
    if (channel == FBZZ_MASK_LUMINANCE_INV) return saturate(1.0f - Luminance(texel.rgb));
    if (channel == FBZZ_MASK_RED)           return saturate(texel.r);
    if (channel == FBZZ_MASK_GREEN)         return saturate(texel.g);
    if (channel == FBZZ_MASK_BLUE)          return saturate(texel.b);
    if (channel == FBZZ_MASK_ALPHA_INV)     return saturate(1.0f - texel.a);
    return saturate(texel.a);
}

// texel の RGB は色として保ったまま、アルファだけを選んだチャンネルで置き換える。
// WHY: 黒背景の炎素材は「RGB が炎の色、輝度が濃さ」という作りになっている。
//      輝度をアルファへ移してもRGBは色として意味を持つため、捨ててはいけない。
float4 ApplyMaskToAlpha(float4 texel, uint channel)
{
    return float4(texel.rgb, ResolveMask(texel, channel));
}

// サンプリングとマスク解決をまとめた入口。
float SampleMask(Texture2D tex, SamplerState samp, float2 uv, uint channel)
{
    return ResolveMask(tex.Sample(samp, uv), channel);
}

// ---- マスクの整形 ---------------------------------------------------------

// [lo, hi] を [0, 1] へ引き伸ばす。lo == hi のときはハードなしきい値になる。
// WHY: 1 枚のマスクから「ふわっとした縁」も「くっきりした縁」も作れるようにする。
//      素材を作り直さずに見た目を詰められることが、実制作では効く。
float RemapMask(float mask, float lo, float hi)
{
    // lo >= hi の指定を許して、その場合はステップ関数として扱う
    // (0 除算を避けつつ「しきい値だけ指定したい」用途にも応える)。
    const float range = hi - lo;
    if (range <= 1.0e-5f) return mask >= lo ? 1.0f : 0.0f;
    return saturate((mask - lo) / range);
}

// 中央 0.5 を軸にコントラストを掛ける。1 で無変化、大きいほど縁が締まる。
float ContrastMask(float mask, float contrast)
{
    return saturate((mask - 0.5f) * contrast + 0.5f);
}

// ---- カットアウト ---------------------------------------------------------

// マスクがしきい値未満なら描画を捨てる。
// WHY: clip(mask - cutoff) は各シェーダーに散らばっていて、
//      「cutoff = 0 のときも clip を通すか」の扱いが揃っていなかった。
//      ここでは cutoff <= 0 を「カットアウト無効」として明示的に素通しする。
void ClipMask(float mask, float cutoff)
{
    if (cutoff <= 0.0f) return;
    clip(mask - cutoff);
}

// ---- 形状マスク (UV から生成) ---------------------------------------------

// 中心 1 / 外周 0 の円形フォールオフ。uv は [0,1] のローカル UV。
// ビルボードの縁を丸く落とす用途で、パーティクルが共通して使う。
float RadialMask(float2 localUv)
{
    const float2 d = localUv * 2.0f - 1.0f;
    const float falloff = saturate(1.0f - dot(d, d));
    // 二乗して中心へ寄せる。線形だと縁が硬く、板の輪郭が見えてしまう。
    return falloff * falloff;
}

// 矩形の縁を border 幅でフェードさせる。デカールやスクリーン演出の縁消し用。
// border は UV 単位 (0.1 なら各辺 10%)。
float EdgeFadeMask(float2 uv, float border)
{
    if (border <= 0.0f) return 1.0f;
    const float2 fromEdge = min(uv, 1.0f - uv);
    const float2 fade = saturate(fromEdge / border);
    return fade.x * fade.y;
}

#endif // FBZZ_MASK_HLSLI
