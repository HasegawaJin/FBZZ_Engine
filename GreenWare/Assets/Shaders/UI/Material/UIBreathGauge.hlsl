/// @file    UIBreathGauge.hlsl
/// @brief   息ゲージを角丸の帯と強調された先端で描く UI シェーダー。
/// @author Hasegawa Jin
/// @date    2026-09-15

#include "UI/UICommon.hlsli"
#include "UI/UIShading.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 fillColor;
    float4 fillHotColor;
    float4 trackColor;
    float4 rimColor;
    float4 cornerRadius;
    float  fillRatio;
    float  fillFromRight;
    float  edgeGlowWidth;
    float  edgeGlowGain;
    float  stripeDensity;
    float  stripeAlpha;
    float  shellThickness;
    float  rimGain;
    float  highlightGain;
    float  _pad0;
    float  _pad1;
};

UIPixelInput VSMain(UIVertexInput input)
{
    return UIVertexMain(input);
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    const float4 texel = g_Texture.Sample(g_Sampler, input.uv);
    const float2 p = UILocalPixels(input.localUv);
    const float2 halfSize = UIHalfSize();
    const float inset = max(shellThickness, 0.0f);
    const float sdOuter = UI_SdRoundedBox(p, halfSize, cornerRadius);
    const float2 innerHalf = max(halfSize - inset, float2(1.0f, 1.0f));
    const float sdInner = UI_SdRoundedBox(p, innerHalf,
                                          max(cornerRadius - inset, 0.0f));

    const float axis = (fillFromRight > 0.5f) ? (1.0f - input.localUv.x)
                                               : input.localUv.x;
    const float head = saturate(fillRatio);
    const float axisPx = (axis - head) * g_Rect.x;
    const float filled = UI_Coverage(axisPx);
    const float upperLight = 1.0f - smoothstep(0.08f, 0.72f, input.localUv.y);
    const float fillLight = saturate(highlightGain * upperLight);
    const float4 chargedColor = lerp(fillColor, fillHotColor, fillLight);
    float4 body = lerp(trackColor, chargedColor, filled);

    if (stripeDensity > 0.0f && stripeAlpha > 0.0f)
    {
        const float stripe = smoothstep(0.38f, 0.5f,
            abs(frac(input.localUv.x * stripeDensity
                     + input.localUv.y * 0.5f) - 0.5f));
        body.rgb = lerp(body.rgb, body.rgb * 1.22f,
                        stripe * stripeAlpha * filled);
    }

    if (edgeGlowWidth > 0.0f && head > 0.001f && head < 0.999f)
    {
        const float glow = saturate(1.0f - abs(axisPx) / edgeGlowWidth);
        body.rgb += fillHotColor.rgb * glow * glow * edgeGlowGain;
    }

    body.a *= UI_Coverage(sdInner);
    float4 shell = float4(rimColor.rgb * rimGain, rimColor.a);
    shell.a *= UI_Border(sdOuter, inset);
    body.a *= texel.a;
    shell.a *= texel.a;
    float4 result = UI_Over(shell, body) * UITint(input);
    result.rgb = UI_Dither(result.rgb, input.pos.xy);
    clip(result.a - 0.002f);
    return result;
}
