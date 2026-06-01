// FBZZ Engine
// PostProcess/Color/Composite.hlsl | PostProcess
// 最終合成パス — HDR + Bloom を合成してトーンマップ・ガンマ補正し LDR に出力する
// フォグ: 深度バッファから線形距離を復元して指数フォグを適用する

#include "Common/Constants.hlsli"
#include "Platform/DX11.hlsli"
#include "Rendering/ToneMap.hlsli"
#include "Rendering/Fog.hlsli"
#include "Rendering/PostProcess.hlsli"

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
    float2 uv = LensDistortUV(p.uv, lensDistortion);
    if (any(uv < 0.0f) || any(uv > 1.0f))
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float2 caOffset = (uv - 0.5f) * chromaticAberration;
    float3 hdr;
    hdr.r = texHDR.Sample(sampDefault, uv + caOffset).r;
    hdr.g = texHDR.Sample(sampDefault, uv).g;
    hdr.b = texHDR.Sample(sampDefault, uv - caOffset).b;
    float3 bloom = texBloom.Sample(sampDefault, uv).rgb;

    // Bloom 加算
    hdr += bloom * bloomIntensity;

    // 露出 → ACES トーンマップ → sRGB ガンマ補正
    float3 ldr = FinalOutput(hdr, exposure);

    // 深度から線形距離を復元して指数フォグを適用する
    // ndcZ ≥ 0.9999 はスカイドーム（clip.xyww で z=w → NDC z=1.0）なので霧を掛けない
    if (fogDensity > 0.0f)
    {
        float ndcZ = texDepth.Sample(sampDefault, uv).r;
        if (ndcZ < 0.9999f)
        {
            float linDepth = nearZ * farZ / (farZ - ndcZ * (farZ - nearZ));
            float dist     = max(linDepth - fogFar, 0.0f);
            float factor   = FogFactor(dist, fogDensity);
            ldr = ApplyFog(ldr, factor, fogColor);
        }
    }

    // 水没カメラ用の全画面補正。
    // WHY: 水面そのものは Water パスで描くが、カメラが水中に入った時の吸収・濁り・視界歪みは
    //      画面全体にかかる効果なので Composite で一括処理する。
    if (underwaterStrength > 0.0f)
    {
        float depthFactor = underwaterStrength;
        float ndcZ = texDepth.Sample(sampDefault, uv).r;
        if (ndcZ < 0.9999f)
        {
            float linDepth = nearZ * farZ / (farZ - ndcZ * (farZ - nearZ));
            depthFactor = saturate((1.0f - exp(-underwaterFogDensity * linDepth)) * underwaterStrength);
        }

        float wave = sin((uv.x + uv.y) * 28.0f + time * 2.4f) * 0.003f * underwaterStrength;
        float2 distortedUV = saturate(uv + float2(wave, wave * 0.5f));
        float3 distortedHdr = texHDR.Sample(sampDefault, distortedUV).rgb;
        float3 distortedLdr = FinalOutput(distortedHdr, exposure);

        ldr = lerp(ldr, distortedLdr, underwaterStrength * 0.25f);
        ldr = lerp(ldr, underwaterColor, depthFactor);

        float gray = dot(ldr, float3(0.299f, 0.587f, 0.114f));
        ldr = lerp(float3(gray, gray, gray), ldr, lerp(1.0f, 0.55f, underwaterStrength));
    }

    ldr = ApplyColorAdjustments(ldr, contrast, saturation, hueShift, temperature, tint);
    ldr = ApplyVignette(ldr, uv, vignetteIntensity, vignetteSmoothness, vignetteRoundness, vignetteColor);
    ldr = ApplyFilmGrain(ldr, uv, filmGrainIntensity, filmGrainResponse);

    return float4(ldr, 1.0f);
}
