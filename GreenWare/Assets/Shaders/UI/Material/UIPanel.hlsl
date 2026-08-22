// FBZZ Engine
// Material/UIPanel.hlsl | UI
// 角丸パネル。枠線・内側グラデーション・落ち影をテクスチャ無しで描く。
//
// UI マテリアルの書き方の見本でもある。守るのは 3 点だけ:
//   1. UI/UICommon.hlsli を include する (Common/Constants.hlsli は不可)
//   2. パラメータは cbuffer MaterialConstants : register(CB_MATERIAL) に置く
//      — 名前を変えるとリフレクションが見つけられず .mat の params が効かない
//   3. VSMain は UIVertexMain をそのまま返す
//
// 対応する .mat は render_path = "ui" を宣言する。
#include "UI/UICommon.hlsli"
#include "UI/UIShading.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 fillColor;       // 面の色 (g_Color と乗算)            offset  0
    float4 borderColor;     // 枠線の色                           offset 16
    float4 cornerRadius;    // 角丸 px: 左上/右上/右下/左下        offset 32
    float  borderWidth;     // 枠線の幅 px                        offset 48
    float  gradientStrength;// 面の明暗差 (0 = 単色)               offset 52
    float  gradientAngle;   // 明暗の向き (ラジアン)               offset 56
    float  shadowSoftness;  // 落ち影のぼけ幅 px (0 = 影なし)      offset 60
    float2 shadowOffset;    // 落ち影のずれ px                    offset 64
    float  shadowAlpha;     // 落ち影の濃さ                       offset 72
    float  _pad0;           //                                    offset 76
};

UIPixelInput VSMain(UIVertexInput input)
{
    return UIVertexMain(input);
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    float2 p        = UILocalPixels(input.localUv);
    float2 halfSize = UIHalfSize();

    // WHY 影のぶんだけ内側へ縮めないか: 影は矩形の外へ広がるが、UI の矩形は
    //     Canvas 上の当たり判定でもある。面を縮めると見た目と判定がずれる。
    //     影は矩形内に収まる範囲だけ出す前提で、offset と softness を小さく使う。
    float distance = UI_SdRoundedBox(p, halfSize, cornerRadius);

    float4 result = float4(0.0f, 0.0f, 0.0f, 0.0f);

    if (shadowSoftness > 0.0f && shadowAlpha > 0.0f)
    {
        float shadow = UI_Shadow(p, halfSize, cornerRadius, shadowOffset, shadowSoftness);
        result = float4(0.0f, 0.0f, 0.0f, shadow * shadowAlpha);
    }

    // 面。グラデーションは明度の乗算で作る。色を 2 つ持たせると
    // 「同じ色の濃淡」を作るのに毎回 2 箇所直すことになる。
    float  gradient = UI_LinearGradient(input.localUv, gradientAngle);
    float  shade    = 1.0f + (gradient - 0.5f) * gradientStrength;
    float4 fill     = fillColor * g_Color;
    fill.rgb       *= shade;
    fill.a         *= UI_Coverage(distance);
    result          = UI_Over(fill, result);

    if (borderWidth > 0.0f && borderColor.a > 0.0f)
    {
        float4 border = borderColor;
        border.a *= UI_Border(distance, borderWidth) * g_Color.a;
        result = UI_Over(border, result);
    }

    // 長いグラデーションは 8bit だと必ず縞になる。量子化 1 段未満で溶かす。
    result.rgb = UI_Dither(result.rgb, input.pos.xy);

    clip(result.a - 0.002f);
    return result;
}
