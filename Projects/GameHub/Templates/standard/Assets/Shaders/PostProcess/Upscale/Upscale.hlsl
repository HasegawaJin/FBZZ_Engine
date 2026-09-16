/// @file    Upscale.hlsl
/// @brief   内部解像度で仕上がった LDR 画を出力先の実寸へ引き伸ばす (Catmull-Rom 9 タップ)。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// b5 の screenSize / texelSize には «入力» の寸法が入る。出力側の寸法は要らない
/// (重みは入力テクセル格子の上で組み立てるため)。

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texSource, TEX_GBUFFER0_SLOT);
// 全画面フェッチなので clamp 必須 (s0 は DX12 では WRAP)。
SamplerState sampLinear : register(SAMPLER_LINEAR_CLAMP);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    const float2 srcSize  = screenSize;
    const float2 srcTexel = texelSize;

    const float2 samplePos = p.uv * srcSize;
    const float2 texPos1   = floor(samplePos - 0.5f) + 0.5f;
    const float2 f         = samplePos - texPos1;

    const float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    const float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    const float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    const float2 w3 = f * f * (-0.5f + 0.5f * f);

    // 中央 2 タップは重み比の位置で 1 回サンプルすれば足りる (16 タップ → 9 タップ)。
    const float2 w12      = w1 + w2;
    const float2 offset12 = w2 / max(w12, 1e-5f);

    const float2 texPos0  = (texPos1 - 1.0f)     * srcTexel;
    const float2 texPos3  = (texPos1 + 2.0f)     * srcTexel;
    const float2 texPos12 = (texPos1 + offset12) * srcTexel;

    float4 result = 0.0f;
    result += texSource.SampleLevel(sampLinear, float2(texPos0.x,  texPos0.y),  0) * (w0.x  * w0.y);
    result += texSource.SampleLevel(sampLinear, float2(texPos12.x, texPos0.y),  0) * (w12.x * w0.y);
    result += texSource.SampleLevel(sampLinear, float2(texPos3.x,  texPos0.y),  0) * (w3.x  * w0.y);

    result += texSource.SampleLevel(sampLinear, float2(texPos0.x,  texPos12.y), 0) * (w0.x  * w12.y);
    result += texSource.SampleLevel(sampLinear, float2(texPos12.x, texPos12.y), 0) * (w12.x * w12.y);
    result += texSource.SampleLevel(sampLinear, float2(texPos3.x,  texPos12.y), 0) * (w3.x  * w12.y);

    result += texSource.SampleLevel(sampLinear, float2(texPos0.x,  texPos3.y),  0) * (w0.x  * w3.y);
    result += texSource.SampleLevel(sampLinear, float2(texPos12.x, texPos3.y),  0) * (w12.x * w3.y);
    result += texSource.SampleLevel(sampLinear, float2(texPos3.x,  texPos3.y),  0) * (w3.x  * w3.y);

    // Catmull-Rom は負の重みを持つので、強い輪郭の外側が沈んでリンギングになる。
    // ここは Composite 後の LDR なので [0,1] で閉じてよい。
    return float4(saturate(result.rgb), 1.0f);
}
