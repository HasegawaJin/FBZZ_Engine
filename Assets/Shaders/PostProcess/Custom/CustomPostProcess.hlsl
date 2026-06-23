// FBZZ Engine
// PostProcess/Custom/CustomPostProcess.hlsl | PostProcess
// Default user-editable full-screen post-process shader

#include "Common/Constants.hlsli"
#include "Platform/DX11.hlsli"
#include "Common/Color.hlsli"

Texture2D    texInput   : register(TEX_GBUFFER0);
SamplerState sampLinear : register(SAMPLER_DEFAULT);

struct VSOut {
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    o.uv         = float2((id & 1u) ? 2.0f : 0.0f, (id & 2u) ? 2.0f : 0.0f);
    o.svPosition = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float3 ApplyDefaultCustomEffect(float3 color, float2 uv)
{
    const float scanlineStrength = saturate(customParameters.x);
    const float grayscaleAmount = saturate(customParameters.y);
    const float redBoost = customParameters.z;
    const float blueBoost = customParameters.w;

    const float scanline = 1.0f - scanlineStrength * 0.5f *
        (0.5f + 0.5f * sin((uv.y * screenSize.y + time * 120.0f) * 3.14159265f));
    const float luma = Luminance(color);
    float3 result = lerp(color, float3(luma, luma, luma), grayscaleAmount);
    result *= scanline;
    result += float3(redBoost, 0.0f, blueBoost) * customIntensity;
    return saturate(result);
}

float4 PSMain(VSOut p) : SV_Target0
{
    const float3 original = texInput.SampleLevel(sampLinear, p.uv, 0).rgb;
    const float3 processed = ApplyDefaultCustomEffect(original, p.uv);
    return float4(lerp(original, processed, saturate(customBlend)), 1.0f);
}
