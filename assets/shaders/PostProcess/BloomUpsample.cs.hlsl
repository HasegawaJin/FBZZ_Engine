// FBZZ Engine
// BloomUpsample.cs.hlsl | PostProcess
// Bloom アップサンプル — テントフィルタで 2 倍解像度に拡大・加算する
//
// Dispatch サイズ: ceil(dstWidth/8) x ceil(dstHeight/8) x 1

#include "Common/Constants.hlsli"
#include "Platform/DX11.hlsli"

Texture2D           texSrc     : register(TEX_BLOOM);
SamplerState        sampDefault: register(SAMPLER_DEFAULT);

RWTexture2D<float4> outputDst  : register(UAV_OUTPUT);
RWTexture2D<float4> outputDst2 : register(UAV_OUTPUT2);  // 加算先 (HDR バッファ)

// テントフィルタ (3x3、重み合計 = 1)
static const float2 TENT_OFFSETS[9] =
{
    float2(-1,-1), float2(0,-1), float2(1,-1),
    float2(-1, 0), float2(0, 0), float2(1, 0),
    float2(-1, 1), float2(0, 1), float2(1, 1),
};
static const float TENT_WEIGHTS[9] =
{
    1.0f/16.0f, 2.0f/16.0f, 1.0f/16.0f,
    2.0f/16.0f, 4.0f/16.0f, 2.0f/16.0f,
    1.0f/16.0f, 2.0f/16.0f, 1.0f/16.0f,
};

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint2  pixel  = dtid.xy;
    float2 dstUV  = (float2(pixel) + 0.5f) * texelSize;
    float2 srcUV  = dstUV * 0.5f;  // ソースは半分解像度

    float4 result = float4(0, 0, 0, 0);
    for (int i = 0; i < 9; ++i)
        result += texSrc.SampleLevel(sampDefault, srcUV + TENT_OFFSETS[i] * texelSize * 2.0f, 0)
                  * TENT_WEIGHTS[i];

    // アップサンプル結果を書き出す (最終パスで HDR バッファに加算される)
    outputDst[pixel] = result;
}
