// FBZZ Engine
// Material/UITextGradient.hlsl | UI
// 文字にグラデーションと縁取りを乗せるテキストマテリアル。
//
// 前提 (組み込みの UIText.hlsl と同じ):
//   t0 はフォントアトラスで、`.r` がカバレッジ。SDF ではないので
//   距離場前提のしきい値処理をしてはいけない。
//
// 制約: g_Rect は 0 が渡る (1 ドローに複数グリフを詰めるため矩形が定まらない)。
//       グラデーションはグリフ内の UV ではなくアトラス UV 基準になるため、
//       縦方向のグラデーションだけが意図どおりに出る。
#include "UI/UICommon.hlsli"
#include "UI/UIShading.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 topColor;        // 文字の上端の色              offset  0
    float4 bottomColor;     // 文字の下端の色              offset 16
    float4 outlineColor;    // 縁取りの色                  offset 32
    float  outlineWidth;    // 縁取りの太さ (画面 px)      offset 48
    float  gradientPower;   // 上下の寄り (1 = 線形)       offset 52
    float  _pad0;           //                             offset 56
    float  _pad1;           //                             offset 60
};

UIPixelInput VSMain(UIVertexInput input)
{
    UIPixelInput output;
    output.pos     = mul(float4(input.pos, 0.0f, 1.0f), g_Ortho);
    output.uv      = input.uv;
    output.localUv = input.uv;
    return output;
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    float coverage = g_Texture.Sample(g_Sampler, input.uv).r;

    // 組み込みと同じく、傾斜を画面 1 ピクセル幅へ正規化してから閾値を取る。
    // ここを smoothstep の固定幅にすると、拡大時に縁が階段状になる。
    float width = max(fwidth(coverage), 1e-4f);
    float alpha = saturate((coverage - 0.5f) / width + 0.5f);

    // 縁取り: 本体より外側の、カバレッジがまだ立っている帯。
    // WHY カバレッジを 2 段のしきい値で切るか: アトラスは 1 テクセル幅の傾斜しか
    //     持たないので、太い縁は作れない。outlineWidth は「傾斜のどこで切るか」で、
    //     画面上の実寸としては 1px 前後が上限になる。それ以上が要るなら
    //     アトラス生成側で膨張させたページを別スロットへ持つこと。
    float outlineAlpha = 0.0f;
    if (outlineWidth > 0.0f && outlineColor.a > 0.0f) {
        float outer = saturate((coverage - 0.5f) / width + 0.5f + outlineWidth);
        outlineAlpha = saturate(outer - alpha);
    }

    float t = saturate(pow(saturate(input.uv.y), max(gradientPower, 1e-3f)));
    float3 body = lerp(topColor.rgb, bottomColor.rgb, t);

    float4 result = float4(body, topColor.a * alpha);
    if (outlineAlpha > 0.0f) {
        float4 outline = float4(outlineColor.rgb, outlineColor.a * outlineAlpha);
        result = UI_Over(result, outline);
    }
    result *= g_Color;

    clip(result.a - 0.002f);
    return result;
}
