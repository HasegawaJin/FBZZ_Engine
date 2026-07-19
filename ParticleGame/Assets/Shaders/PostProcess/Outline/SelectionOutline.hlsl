// FBZZ Engine
// PostProcess/Outline/SelectionOutline.hlsl | PostProcess
// Pixel-width editor selection outline from a selected-object mask

#include "Common/Constants.hlsli"
#include "Platform/Backend.hlsli"

Texture2D          texLDR            : register(TEX_GBUFFER0);
Texture2D          texSelectionMask  : register(TEX_GBUFFER1);
Texture2D<float>   texSceneDepth     : register(TEX_DEPTH);
Texture2D<float>   texSelectionDepth : register(TEX_SHADOW);
SamplerState       sampLinear        : register(SAMPLER_DEFAULT);

cbuffer OutlineConstants : register(CB_MATERIAL)
{
    float4 outlineColor;
    float  outlineWidth;
    float3 _outlinePad;
};

struct VSOut
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    o.uv         = float2((id & 1u) ? 2.0f : 0.0f,
                          (id & 2u) ? 2.0f : 0.0f);
    o.svPosition = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float4 PSMain(VSOut p) : SV_Target0
{
    float3 baseColor = texLDR.SampleLevel(sampLinear, p.uv, 0).rgb;
    float centerMask = texSelectionMask.SampleLevel(sampLinear, p.uv, 0).r;

    float radiusPx = clamp(outlineWidth * 100.0f, 1.0f, 16.0f);
    int radius = (int)ceil(radiusPx);

    float maxMask = centerMask;
    float selectedDepth = 1.0f;

    [loop]
    for (int y = -16; y <= 16; ++y)
    {
        [loop]
        for (int x = -16; x <= 16; ++x)
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
