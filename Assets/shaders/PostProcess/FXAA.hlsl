// FBZZ Engine
// FXAA.hlsl | PostProcess
// Fast Approximate Anti-Aliasing — FXAA 3.11 console 版の PS 移植
// 入力: TEX_GBUFFER0 (t5) = Composite 出力 LDR カラー

#include "Common/Constants.hlsli"
#include "Platform/DX11.hlsli"

Texture2D    texLDR     : register(TEX_GBUFFER0);
SamplerState sampLinear : register(SAMPLER_DEFAULT);

static const float FXAA_SPAN_MAX   = 8.0f;
static const float FXAA_REDUCE_MUL = 1.0f / 8.0f;
static const float FXAA_REDUCE_MIN = 1.0f / 128.0f;

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

float Luma(float3 rgb) { return dot(rgb, float3(0.299f, 0.587f, 0.114f)); }

float4 PSMain(VSOut p) : SV_Target0
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
