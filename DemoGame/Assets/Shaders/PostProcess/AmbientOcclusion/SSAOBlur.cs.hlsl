// FBZZ Engine
// PostProcess/AmbientOcclusion/SSAOBlur.cs.hlsl | PostProcess
// SSAO の 4x4 ボックスブラー — ノイズを平滑化する
//
// Dispatch サイズ: ceil(width/8) x ceil(height/8) x 1

#include "Common/Constants.hlsli"
#include "Platform/DX11.hlsli"

Texture2D          texSSAO     : register(TEX_SSAO);
SamplerState       sampDefault : register(SAMPLER_DEFAULT);

RWTexture2D<float4> outputBlur : register(UAV_OUTPUT);

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint2  pixel = dtid.xy;
    if (pixel.x >= (uint)screenSize.x || pixel.y >= (uint)screenSize.y)
        return;

    float2 uv    = (float2(pixel) + 0.5f) * texelSize;

    float result = 0.0f;
    for (int y = -2; y <= 1; ++y)
    for (int x = -2; x <= 1; ++x)
    {
        int2 samplePixel = clamp(int2(pixel) + int2(x, y),
                                 int2(0, 0), int2(screenSize) - int2(1, 1));
        result += texSSAO.Load(int3(samplePixel, 0)).r;
    }

    const float ao = result / 16.0f;
    outputBlur[pixel] = float4(ao, ao, ao, 1.0f);
}
