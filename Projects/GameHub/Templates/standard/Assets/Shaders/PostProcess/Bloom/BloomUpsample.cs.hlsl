/// @file BloomUpsample.cs.hlsl
/// @brief Bloom ミップ連鎖のアップサンプル (3x3 テントで拡大し、書き込み先へ加算する)
/// @author Hasegawa Jin
/// @date 2026-08-25
//
// Dispatch サイズ: ceil(dstWidth/8) x ceil(dstHeight/8) x 1
//
// texelSize     = 書き込み先のテクセルサイズ
// bloomSrcTexel = 読み込み元 (1 段小さいミップ) のテクセルサイズ
// bloomAdditive = 1 で texAdd (同じ寸法のダウンサンプル結果) を足す。
//
// WHY 書き込み先の UAV を読んで加算しないか: コンピュート用テクスチャは RGBA16F で、
//     D3D11 で typed UAV load が保証されるのは R32 系だけ。RWTexture2D<float4> を
//     読むと未定義動作になり、実際に画面が 2x2 に割れた。足すものは SRV から読む。

#include "Common/Constants.hlsli"
#include "Platform/Backend.hlsli"

Texture2D           texSrc     : register(TEX_BLOOM);      // 1 段小さいミップ
Texture2D           texAdd     : register(TEX_BLOOM_ADD);  // 同じ寸法のダウンサンプル結果
// 全画面フェッチなので clamp 必須 (s0 は DX12 では WRAP)。
SamplerState        sampDefault: register(SAMPLER_LINEAR_CLAMP);

RWTexture2D<float4> outputDst  : register(UAV_OUTPUT);

// 3x3 テント (重み合計 = 1)
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
    uint2 pixel = dtid.xy;
    uint2 outputSize;
    outputDst.GetDimensions(outputSize.x, outputSize.y);
    if (any(pixel >= outputSize))
        return;

    // 書き込み先の画素中心 → 正規化 UV。読み込み元は解像度が違うだけで
    // UV 空間は共通なので、そのままサンプルしてよい。
    const float2 uv = (float2(pixel) + 0.5f) * texelSize;

    float4 result = 0.0f;
    [unroll] for (int i = 0; i < 9; ++i)
        result += texSrc.SampleLevel(sampDefault, uv + TENT_OFFSETS[i] * bloomSrcTexel, 0)
                * TENT_WEIGHTS[i];

    // 途中段は、この段のダウンサンプル結果へより広いぼけを重ねる。
    // 全解像度への最終段だけ加算せずそのまま書く。
    if (bloomAdditive > 0.5f)
        result += texAdd.SampleLevel(sampDefault, uv, 0);

    outputDst[pixel] = result;
}
