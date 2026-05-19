// FBZZ Engine
// Composite.hlsl | PostProcess
// 最終合成パス — HDR + Bloom を合成してトーンマップ・ガンマ補正し LDR に出力する
// フォグ: 深度バッファから線形距離を復元して指数フォグを適用する

#include "Common/Constants.hlsli"
#include "Platform/DX11.hlsli"
#include "Rendering/ToneMap.hlsli"
#include "Rendering/Fog.hlsli"

Texture2D          texHDR      : register(TEX_GBUFFER0);  // ライティング結果 HDR バッファ
Texture2D          texBloom    : register(TEX_BLOOM);
Texture2D<float>   texDepth    : register(TEX_DEPTH);     // 深度 (フォグ計算用)
SamplerState       sampDefault : register(SAMPLER_DEFAULT);

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

    // Bloom 加算
    hdr += bloom * 0.04f;

    // 露出 → ACES トーンマップ → sRGB ガンマ補正
    float3 ldr = FinalOutput(hdr, exposure);

    // 深度から線形距離を復元して指数フォグを適用する
    // ndcZ ≥ 0.9999 はスカイドーム（clip.xyww で z=w → NDC z=1.0）なので霧を掛けない
    if (fogDensity > 0.0f)
    {
        float ndcZ = texDepth.Sample(sampDefault, p.uv).r;
        if (ndcZ < 0.9999f)
        {
            float linDepth = nearZ * farZ / (farZ - ndcZ * (farZ - nearZ));
            float dist     = max(linDepth - fogFar, 0.0f);
            float factor   = FogFactor(dist, fogDensity);
            ldr = ApplyFog(ldr, factor, fogColor);
        }
    }

    return float4(ldr, 1.0f);
}
