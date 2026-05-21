// FBZZ Engine
// BloomDownsample.cs.hlsl | PostProcess
// Bloom ダウンサンプル — 輝度閾値でフィルタしながら半分解像度に縮小する
//
// Dispatch サイズ: ceil(dstWidth/8) x ceil(dstHeight/8) x 1

#include "Common/Constants.hlsli"
#include "Common/Color.hlsli"
#include "Platform/DX11.hlsli"

Texture2D          texSrc      : register(TEX_BLOOM);
SamplerState       sampDefault : register(SAMPLER_DEFAULT);

RWTexture2D<float4> outputDst  : register(UAV_OUTPUT);

// 輝度がこの値を超えたピクセルだけを Bloom に含める
static const float BLOOM_THRESHOLD = 0.7f;

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint2  pixel  = dtid.xy;
    // 出力テクセルサイズ (入力の 2 倍)
    float2 srcUV  = (float2(pixel) + 0.5f) * texelSize * 2.0f;

    // 2x2 バイリニア平均
    float4 s0 = texSrc.SampleLevel(sampDefault, srcUV + texelSize * float2(-0.5f, -0.5f), 0);
    float4 s1 = texSrc.SampleLevel(sampDefault, srcUV + texelSize * float2( 0.5f, -0.5f), 0);
    float4 s2 = texSrc.SampleLevel(sampDefault, srcUV + texelSize * float2(-0.5f,  0.5f), 0);
    float4 s3 = texSrc.SampleLevel(sampDefault, srcUV + texelSize * float2( 0.5f,  0.5f), 0);
    float4 avg = (s0 + s1 + s2 + s3) * 0.25f;

    // 輝度閾値
    float lum    = Luminance(avg.rgb);
    float weight = max(lum - BLOOM_THRESHOLD, 0.0f) / max(lum, 0.0001f);

    outputDst[pixel] = float4(avg.rgb * weight, avg.a);
}
