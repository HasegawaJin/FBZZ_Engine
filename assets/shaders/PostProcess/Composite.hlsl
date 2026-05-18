// FBZZ Engine
// Composite.hlsl | PostProcess
// 最終合成パス — HDR + Bloom を合成してトーンマップ・ガンマ補正し LDR に出力する

#include "Common/Constants.hlsli"
#include "Platform/DX11.hlsli"
#include "Rendering/ToneMap.hlsli"

Texture2D    texHDR      : register(TEX_GBUFFER0);  // ライティング結果 HDR バッファ
Texture2D    texBloom    : register(TEX_BLOOM);
SamplerState sampDefault : register(SAMPLER_DEFAULT);

struct FSTriVSOut
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

FSTriVSOut VSMain(uint id : SV_VertexID)
{
    FSTriVSOut o;
    o.uv         = float2((id & 1u) ? 2.0f : 0.0f,
                          (id & 2u) ? 2.0f : 0.0f);
    o.svPosition = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float4 PSMain(FSTriVSOut p) : SV_Target0
{
    float3 hdr   = texHDR.Sample(sampDefault, p.uv).rgb;
    float3 bloom = texBloom.Sample(sampDefault, p.uv).rgb;

    // Bloom 加算 (強度は固定 0.04; PostProcConstants への追加で調整可)
    hdr += bloom * 0.04f;

    // 露出 → ACES トーンマップ → sRGB ガンマ補正
    return float4(FinalOutput(hdr, exposure), 1.0f);
}
