// FBZZ Engine
// PostProcess/Outline/SelectionOutline.hlsl | PostProcess
// Pixel-width editor selection outline from a selected-object mask

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Platform/Backend.hlsli"

Texture2D          texLDR            : register(TEX_GBUFFER0);
Texture2D          texSelectionMask  : register(TEX_GBUFFER1);
Texture2D<float>   texSceneDepth     : register(TEX_DEPTH);
Texture2D<float>   texSelectionDepth : register(TEX_SHADOW);
// 全画面フェッチなので clamp 必須 (s0 は DX12 では WRAP)。
SamplerState       sampLinear        : register(SAMPLER_LINEAR_CLAMP);

cbuffer OutlineConstants : register(CB_MATERIAL)
{
    float4 outlineColor;
    float  outlineWidth;
    float3 _outlinePad;
};

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    float3 baseColor = texLDR.SampleLevel(sampLinear, p.uv, 0).rgb;
    float centerMask = texSelectionMask.SampleLevel(sampLinear, p.uv, 0).r;

    float radiusPx = clamp(outlineWidth * 100.0f, 1.0f, 16.0f);
    int radius = (int)ceil(radiusPx);

    float maxMask = centerMask;
    float selectedDepth = 1.0f;

    // 走査範囲は radius (= ceil(radiusPx)) まで。半径外は下の distPx 判定で必ず弾かれるので
    // 出力は固定 33x33 走査と完全に一致する。
    // WHY: 固定 -16..16 だと輪郭幅 1px でも 1 ピクセルあたり 1089 回ループを回していた。
    [loop]
    for (int y = -radius; y <= radius; ++y)
    {
        [loop]
        for (int x = -radius; x <= radius; ++x)
        {
            float2 offsetPx = float2((float)x, (float)y);
            float distPx = length(offsetPx);
            if (distPx > radiusPx) continue;

            float2 sampleUv = p.uv + offsetPx * texelSize;
            float mask = texSelectionMask.SampleLevel(sampLinear, sampleUv, 0).r;
            if (mask > maxMask)
            {
                maxMask = mask;
            }
            if (mask > 0.5f)
            {
                selectedDepth = min(selectedDepth, texSelectionDepth.SampleLevel(sampLinear, sampleUv, 0).r);
            }
        }
    }

    float edge = saturate(maxMask - centerMask);
    float sceneDepth = texSceneDepth.SampleLevel(sampLinear, p.uv, 0).r;
    float visible = selectedDepth <= sceneDepth + 0.0002f ? 1.0f : 0.0f;
    float alpha = edge * outlineColor.a * visible;
    float3 color = lerp(baseColor, outlineColor.rgb, alpha);
    return float4(color, 1.0f);
}
