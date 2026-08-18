// FBZZ Engine
// UIText.hlsl | UI
// フォントアトラス (カバレッジ) テキストシェーダー
//
// アトラスの中身:
//   gen_font_atlas.py が Pillow で TTF をグレースケール描画した「カバレッジ」テクスチャ。
//   .r = 0.0 (グリフ外) 〜 1.0 (グリフ内) で、縁はアンチエイリアスされた約 1 テクセル幅の傾斜。
//   SDF (符号付き距離場) ではないので、距離場前提のしきい値処理をしてはいけない。
cbuffer UIConstants : register(b0)
{
    float4x4 g_Ortho;
    float4   g_Color;
    float4   g_UVRect;
};

Texture2D    g_Texture : register(t0);
SamplerState g_Sampler : register(s5);

struct VSIn
{
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct PSIn
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

PSIn VSMain(VSIn input)
{
    PSIn output;
    output.pos = mul(float4(input.pos, 0.0f, 1.0f), g_Ortho);
    output.uv  = input.uv;
    return output;
}

float4 PSMain(PSIn input) : SV_TARGET
{
    // アトラスの .r をカバレッジとして読む (バイリニア補間済み)。
    float coverage = g_Texture.Sample(g_Sampler, input.uv).r;

    // WHY: 旧実装は smoothstep(0.35, 0.65, coverage) を掛けていた。これは coverage を
    //      SDF と誤認した処理で、生成時に作られた 1 テクセル幅の AA 傾斜を 30% 幅へ圧縮し、
    //      拡大表示 (アトラス 48px に対し fontSize 84 等) では傾斜がサブピクセル未満に潰れて
    //      ほぼ二値化 → 縁が階段状 (ジャギー) になっていた。
    //
    //      代わりに傾斜を「画面 1 ピクセル幅」へ正規化し直す。
    //      fwidth(coverage) は隣接ピクセル間のカバレッジ変化量なので、
    //      (coverage - 0.5) / fwidth(coverage) は縁からの符号付き距離をピクセル単位で表す。
    //      これに 0.5 を足して saturate すると、拡大時は縁が締まり (ボケない)、
    //      縮小時は自動的に広くなって滑らかに減衰する ─ 常に 1px 幅の AA が得られる。
    //
    //      max() の下限はグリフ内部/外部の平坦部 (fwidth == 0) でのゼロ除算を防ぐためのもの。
    //      平坦部では商が ±巨大値になり、saturate で正しく 1.0 / 0.0 に落ちる。
    float width = max(fwidth(coverage), 1e-4f);
    float alpha = saturate((coverage - 0.5f) / width + 0.5f);

    // 完全透明なピクセルは破棄してブレンド/オーバードローを減らす。
    // fwidth は clip より前に評価済みなので、破棄が微分に影響することはない。
    clip(alpha - 0.002f);
    return float4(g_Color.rgb, g_Color.a * alpha);
}
