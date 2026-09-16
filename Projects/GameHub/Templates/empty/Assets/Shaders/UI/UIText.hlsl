/// @file UIText.hlsl
/// @brief フォントアトラスのテキストシェーダー
/// @author Hasegawa Jin
/// @date 2026-05-23
///
/// アトラスの中身は 2 種類あるが、PSMain の 1 式が両方を扱う。
///   静的 (.fnt)           … カバレッジ。0.0 (グリフ外) 〜 1.0 (グリフ内)、縁は約 1 テクセル幅の傾斜
///   動的 (.ttf を直接指定) … 単一チャンネル SDF。stb_truetype の onedge_value=128 が UNORM で 0.502
/// どちらも「.r の 0.5 が輪郭」で一致するため、種別で分岐する必要がない。
/// MSDF (RGB の中央値が距離) だけはこの式で扱えない。入れるなら別シェーダーになる。
#include "UI/UICommon.hlsli"

// WHY 共通の UIVertexMain を使わないか:
//   テキストの頂点バッファはグリフごとにアトラス UV を直接持っている。
//   矩形 1 枚を 0..1 で張るスプライトと違い、ここで g_UVRect を掛けると
//   グリフ位置が二重に写像されて別の文字を拾う。
UIPixelInput VSMain(UIVertexInput input)
{
    UIPixelInput output;
    output.pos     = mul(float4(input.pos, 0.0f, 1.0f), g_Ortho);
    output.uv      = input.uv;
    output.localUv = input.uv;
    // リッチテキストの色はここを通る。1 ドローに詰めた文字ごとに違う値が乗る。
    output.color   = input.color;
    return output;
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    // 0.5 を輪郭とする場 (カバレッジまたは SDF) を読む。バイリニア補間済み。
    float field = g_Texture.Sample(g_Sampler, input.uv).r;

    // WHY: 旧実装は smoothstep(0.35, 0.65, ...) を掛けていた。カバレッジしか無かった当時に
    //      それを SDF と誤認した処理で、生成時に作られた 1 テクセル幅の AA 傾斜を 30% 幅へ圧縮し、
    //      拡大表示 (アトラス 48px に対し fontSize 84 等) では傾斜がサブピクセル未満に潰れて
    //      ほぼ二値化 → 縁が階段状 (ジャギー) になっていた。
    //
    //      代わりに傾斜を「画面 1 ピクセル幅」へ正規化し直す。
    //      fwidth(field) は隣接ピクセル間の変化量なので、(field - 0.5) / fwidth(field) は
    //      縁からの符号付き距離をピクセル単位で表す。
    //      これに 0.5 を足して saturate すると、拡大時は縁が締まり (ボケない)、
    //      縮小時は自動的に広くなって滑らかに減衰する ─ 常に 1px 幅の AA が得られる。
    //
    //      この式が距離場にもそのまま効くことが、動的 SDF アトラスを分岐無しで
    //      同居させられている理由でもある。
    //
    //      max() の下限はグリフ内部/外部の平坦部 (fwidth == 0) でのゼロ除算を防ぐためのもの。
    //      平坦部では商が ±巨大値になり、saturate で正しく 1.0 / 0.0 に落ちる。
    float width = max(fwidth(field), 1e-4f);
    float alpha = saturate((field - 0.5f) / width + 0.5f);

    // 完全透明なピクセルは破棄してブレンド/オーバードローを減らす。
    // fwidth は clip より前に評価済みなので、破棄が微分に影響することはない。
    clip(alpha - 0.002f);
    float4 tint = UITint(input);
    return float4(tint.rgb, tint.a * alpha);
}
