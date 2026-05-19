// FBZZ Engine
// SSAOBlur.cs.hlsl | PostProcess
// SSAO の 4x4 ボックスブラー — ノイズを平滑化する
//
// Dispatch サイズ: ceil(width/8) x ceil(height/8) x 1

#include "Common/Constants.hlsli"
#include "Platform/DX11.hlsli"

Texture2D<float>   texSSAO     : register(TEX_SSAO);
SamplerState       sampDefault : register(SAMPLER_DEFAULT);

RWTexture2D<float> outputBlur  : register(UAV_OUTPUT);

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint2  pixel = dtid.xy;
    float2 uv    = (float2(pixel) + 0.5f) * texelSize;

    float result = 0.0f;
    for (int y = -2; y <= 1; ++y)
    for (int x = -2; x <= 1; ++x)
    {
        float2 offset = float2(x, y) * texelSize;
        result += texSSAO.SampleLevel(sampDefault, uv + offset, 0);
    }

    outputBlur[pixel] = result / 16.0f;
}
