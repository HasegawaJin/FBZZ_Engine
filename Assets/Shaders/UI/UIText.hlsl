/// @file UIText.hlsl
/// @brief フォントアトラスのテキストシェーダー
/// @author Hasegawa Jin
/// @date 2026-05-23

/// @note アトラスの中身は 2 種類あるが、PSMain の 1 式が両方を扱う。
/// @note 静的 (.fnt)           … カバレッジ。0.0 (グリフ外) 〜 1.0 (グリフ内)、縁は約 1 テクセル幅の傾斜
/// @note 動的 (.ttf を直接指定) … 単一チャンネル SDF。stb_truetype の onedge_value=128 が UNORM で 0.502
/// @note どちらも「.r の 0.5 が輪郭」で一致するため、種別で分岐する必要がない。
/// @note MSDF (RGB の中央値が距離) だけはこの式で扱えない。入れるなら別シェーダーになる。
#include "UI/UICommon.hlsli"

/// @note 共通の UIVertexMain を使わないか:
/// @note テキストの頂点バッファはグリフごとにアトラス UV を直接持っている。
/// @note 矩形 1 枚を 0..1 で張るスプライトと違い、ここで g_UVRect を掛けると
/// @note グリフ位置が二重に写像されて別の文字を拾う。
UIPixelInput VSMain(UIVertexInput input)
{
    return UIAtlasVertexMain(input);
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    /// @note 0.5 を輪郭とする場 (カバレッジまたは SDF) を読む。バイリニア補間済み。
    float field = g_Texture.Sample(g_Sampler, input.uv).r;

    /// @note 旧実装は smoothstep(0.35, 0.65, ...) を掛けていた。カバレッジしか無かった当時に
    /// @note それを SDF と誤認した処理で、生成時に作られた 1 テクセル幅の AA 傾斜を 30% 幅へ圧縮し、
    /// @note 拡大表示 (アトラス 48px に対し fontSize 84 等) では傾斜がサブピクセル未満に潰れて
    /// @note ほぼ二値化 → 縁が階段状 (ジャギー) になっていた。

    /// @note 代わりに傾斜を「画面 1 ピクセル幅」へ正規化し直す。
    /// @note fwidth(field) は隣接ピクセル間の変化量なので、(field - 0.5) / fwidth(field) は
    /// @note 縁からの符号付き距離をピクセル単位で表す。
    /// @note これに 0.5 を足して saturate すると、拡大時は縁が締まり (ボケない)、
    /// @note 縮小時は自動的に広くなって滑らかに減衰する ─ 常に 1px 幅の AA が得られる。

    /// @note この式が距離場にもそのまま効くことが、動的 SDF アトラスを分岐無しで
    /// @note 同居させられている理由でもある。

    /// @note max() の下限はグリフ内部/外部の平坦部 (fwidth == 0) でのゼロ除算を防ぐためのもの。
    /// @note 平坦部では商が ±巨大値になり、saturate で正しく 1.0 / 0.0 に落ちる。
    float width = max(fwidth(field), 1e-4f);
    float alpha = saturate((field - 0.5f) / width + 0.5f);

    /// @note 完全透明なピクセルは破棄してブレンド/オーバードローを減らす。
    /// @note fwidth は clip より前に評価済みなので、破棄が微分に影響することはない。
    clip(alpha - 0.002f);
    float4 tint = UITint(input);
    return float4(tint.rgb, tint.a * alpha);
}
