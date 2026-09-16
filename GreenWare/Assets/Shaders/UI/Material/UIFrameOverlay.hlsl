/// @file    UIFrameOverlay.hlsl
/// @brief   HUD 枠へ発光リムと走査光を重ねる UI オーバーレイ。
/// @author Hasegawa Jin
/// @date    2026-09-15

#include "UI/UICommon.hlsli"
#include "UI/UIShading.hlsli"

cbuffer MaterialConstants : register(CB_MATERIAL)
{
    float4 overlayColor;
    float4 rimColor;
    float4 metalColor;
    float4 cornerRadius;
    float  borderWidth;
    float  rimGain;
    float  scanDensity;
    float  scanAlpha;
    float  scanWidth;
    float  metallicGain;
    float  brushedDensity;
    float  brushedAlpha;
    float  _pad1;
    float  _pad2;
};

UIPixelInput VSMain(UIVertexInput input)
{
    return UIVertexMain(input);
}

float4 PSMain(UIPixelInput input) : SV_TARGET
{
    const float4 texel = g_Texture.Sample(g_Sampler, input.uv);
    const float textureMask = texel.a;
    const float2 p = UILocalPixels(input.localUv);
    const float2 halfSize = UIHalfSize();
    const float sd = UI_SdRoundedBox(p, halfSize, cornerRadius);
    const float rim = UI_Coverage(UI_Border(sd, max(borderWidth, 0.0f)));

    const float scanPhase = frac(input.localUv.x * scanDensity
                                  + input.localUv.y * 0.35f);
    const float scan = 1.0f - smoothstep(0.0f, max(scanWidth, 0.001f),
                                          abs(scanPhase - 0.5f));
    const float innerMask = UI_Coverage(-sd);
    const float4 tint = UITint(input);
    const float brushed = 1.0f - smoothstep(0.38f, 0.5f,
        abs(frac(input.localUv.y * brushedDensity
                 + input.localUv.x * 0.08f) - 0.5f));
    const float3 metalBase = lerp(texel.rgb,
                                  texel.rgb * metalColor.rgb,
                                  saturate(metallicGain));
    float4 result = float4(metalBase, texel.a) * tint;
    result.rgb += metalColor.rgb * brushed * brushedAlpha * textureMask * tint.rgb;
    result.rgb += rimColor.rgb * rim * rimGain * textureMask * tint.rgb;
    result.rgb += overlayColor.rgb * scan * scanAlpha * innerMask
                * textureMask * tint.rgb;
    result.a = max(result.a, rim * rimColor.a * textureMask * tint.a);
    result.rgb = UI_Dither(result.rgb, input.pos.xy);
    clip(result.a - 0.002f);
    return result;
}
