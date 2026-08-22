// FBZZ Engine
// Material/UIGauge.hlsl | UI
// 角丸ゲージ。充填量を UIImage.fillAmount ではなくマテリアル側で解釈する。
//
// WHY fillAmount を使わないか:
//   UISystem の fillAmount は矩形と UV を切り落とす実装で、角丸の内側でも
//   まっすぐ切れる。ゲージの先端が角を跨いだときに角の形が消えるうえ、
//   先端を光らせる・目盛りを刻むといった表現の入る余地が無い。
//   充填量をパラメータで受け取れば、先端の形も発光も距離場の中で決められる。
#include "UI/UICommon.hlsli"
#include "UI/UIShading.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 fillColor;      // 満ちている側の色               offset  0
    float4 trackColor;     // 空の側の色                     offset 16
    float4 cornerRadius;   // 角丸 px: 左上/右上/右下/左下    offset 32
    float  fillRatio;      // 充填量 [0,1]                   offset 48
    float  fillFromRight;  // 1 で右詰め (左右対称の HUD 用)  offset 52
    float  edgeGlowWidth;  // 先端の発光幅 px (0 でなし)      offset 56
    float  edgeGlowGain;   // 先端の発光倍率                  offset 60
    float  tickInterval;   // 目盛り間隔 px (0 でなし)        offset 64
    float  tickWidth;      // 目盛り線の幅 px                 offset 68
    float  tickAlpha;      // 目盛り線の濃さ                  offset 72
    float  _pad0;          //                                 offset 76
};

UIPixelInput VSMain(UIVertexInput input)
{
    return UIVertexMain(input);
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    float2 p        = UILocalPixels(input.localUv);
    float2 halfSize = UIHalfSize();
    float  distance = UI_SdRoundedBox(p, halfSize, cornerRadius);

    // 充填の進行方向。右詰めのときは座標を反転させるだけで同じ式が使える。
    float axis  = (fillFromRight > 0.5f) ? (1.0f - input.localUv.x) : input.localUv.x;
    float head  = saturate(fillRatio);

    // 先端は矩形の縁と同じく 1 ピクセル幅で締める。ここを smoothstep の
    // 固定幅にすると、Canvas を縮めたときだけ先端がボケて別物に見える。
    float axisPixels = (axis - head) * g_Rect.x;
    float filled     = UI_Coverage(axisPixels);

    float4 track = trackColor;
    float4 fill  = fillColor * g_Color;
    float4 body  = lerp(track, fill, filled);
    body.a      *= UI_Coverage(distance);

    // 先端の発光。満ちきっている / 空のときは先端が存在しないので出さない。
    if (edgeGlowWidth > 0.0f && head > 0.001f && head < 0.999f)
    {
        float glow = saturate(1.0f - abs(axisPixels) / edgeGlowWidth);
        body.rgb  += fill.rgb * glow * glow * edgeGlowGain;
    }

    // 目盛り。「あと何発ぶんか」を読ませたいゲージでは、長さより刻みが効く。
    if (tickInterval > 0.0f && tickAlpha > 0.0f)
    {
        float positionPixels = axis * g_Rect.x;
        float toTick = abs(frac(positionPixels / tickInterval) - 0.5f) * tickInterval;
        float tick   = UI_Coverage(toTick - tickWidth * 0.5f);
        body.rgb     = lerp(body.rgb, body.rgb * 0.35f, tick * tickAlpha);
    }

    body.rgb = UI_Dither(body.rgb, input.pos.xy);

    clip(body.a - 0.002f);
    return body;
}
