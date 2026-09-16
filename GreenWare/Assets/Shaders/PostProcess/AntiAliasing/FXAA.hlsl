// FBZZ Engine
// PostProcess/AntiAliasing/FXAA.hlsl | PostProcess
// Fast Approximate Anti-Aliasing — FXAA 3.11 console 版の PS 移植
// 入力: TEX_GBUFFER0 (t5) = Composite 出力 LDR カラー

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texLDR, TEX_GBUFFER0_SLOT);
// 全画面フェッチなので clamp 必須 (s0 は DX12 では WRAP)。
SamplerState sampLinear : register(SAMPLER_LINEAR_CLAMP);

static const float FXAA_SPAN_MAX   = 8.0f;
static const float FXAA_REDUCE_MUL = 1.0f / 8.0f;
static const float FXAA_REDUCE_MIN = 1.0f / 128.0f;

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float Luma(float3 rgb) { return dot(rgb, float3(0.299f, 0.587f, 0.114f)); }

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    float2 uv  = p.uv;
    float2 rcp = texelSize;

    // 斜め 4 点 + 中心をサンプル
    float3 cNW = texLDR.SampleLevel(sampLinear, uv + float2(-0.5f, -0.5f) * rcp, 0).rgb;
    float3 cNE = texLDR.SampleLevel(sampLinear, uv + float2( 0.5f, -0.5f) * rcp, 0).rgb;
    float3 cSW = texLDR.SampleLevel(sampLinear, uv + float2(-0.5f,  0.5f) * rcp, 0).rgb;
    float3 cSE = texLDR.SampleLevel(sampLinear, uv + float2( 0.5f,  0.5f) * rcp, 0).rgb;
    float3 cM  = texLDR.SampleLevel(sampLinear, uv, 0).rgb;

    float lumaNW = Luma(cNW), lumaNE = Luma(cNE);
    float lumaSW = Luma(cSW), lumaSE = Luma(cSE);
    float lumaM  = Luma(cM);

    // 輝度勾配からエッジ方向ベクトルを推定
    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25f * FXAA_REDUCE_MUL),
                          FXAA_REDUCE_MIN);
    float rcpDirMin = 1.0f / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin,
                float2(-FXAA_SPAN_MAX, -FXAA_SPAN_MAX),
                float2( FXAA_SPAN_MAX,  FXAA_SPAN_MAX)) * rcp;

    // エッジ方向に 2 段サンプル
    float3 rgbA = 0.5f * (
        texLDR.SampleLevel(sampLinear, uv + dir * (1.0f / 3.0f - 0.5f), 0).rgb +
        texLDR.SampleLevel(sampLinear, uv + dir * (2.0f / 3.0f - 0.5f), 0).rgb);
    float3 rgbB = 0.5f * rgbA + 0.25f * (
        texLDR.SampleLevel(sampLinear, uv + dir * -0.5f, 0).rgb +
        texLDR.SampleLevel(sampLinear, uv + dir *  0.5f, 0).rgb);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    float lumaB   = Luma(rgbB);

    // rgbB が局所コントラスト外にはみ出た場合は rgbA を採用
    float3 result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
    return float4(result, 1.0f);
}
