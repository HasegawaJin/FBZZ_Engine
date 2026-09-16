// FBZZ Engine
// PostProcess/Water/Caustics.hlsl | PostProcess
// 深度から復元した水面下のワールド座標へコースティクスを投影する

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Platform/Backend.hlsli"

Texture2D        g_causticsTex : register(TEX_ALBEDO);
Texture2D<float> g_depth       : register(TEX_DEPTH);
// コースティクス模様はワールド座標でタイリングさせるので wrap。
SamplerState     sampTiling    : register(SAMPLER_DEFAULT);
// 深度は全画面フェッチなので clamp。s0 と分けるのは、DX12 では s0 が WRAP に固定で、
// 同じサンプラーで両方引くとどちらかが必ず間違うため。
SamplerState     sampDepth     : register(SAMPLER_LINEAR_CLAMP);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    float ndcDepth = g_depth.Sample(sampDepth, p.uv).r;
    if (ndcDepth >= 0.9999f)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    float3 worldPos = ReconstructWorldPos(p.uv, ndcDepth, invViewProjection);
    float waterSurfaceY = customParameters.z;
    float waterDepth = waterSurfaceY - worldPos.y;
    if (waterDepth <= 0.0f)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    // 水域 XZ 範囲フェード: 矩形の外は 0、縁 (約 10%) で滑らかに減衰させ、範囲外の地形へ漏らさない。
    // HalfExtent<=0 のときは無制限 (後方互換)。
    float boundsFade = 1.0f;
    float2 he = float2(causticsHalfExtentX, causticsHalfExtentZ);
    if (he.x > 0.0f && he.y > 0.0f)
    {
        float2 d    = abs(worldPos.xz - float2(causticsCenterX, causticsCenterZ));
        float2 edge = saturate((he - d) / max(he * 0.1f, 1e-3f));
        boundsFade  = min(edge.x, edge.y);
        if (boundsFade <= 0.0f)
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    // 波連動の UV ゆらぎ: 簡易 sin 波で水面屈折による模様変位を近似する (WaveAmp=0 で無効)。
    float2 waveUV   = worldPos.xz * causticsWaveFreq + customParameters.w * causticsWaveSpeed;
    float2 waveDisp = float2(sin(waveUV.x + waveUV.y), cos(waveUV.x - waveUV.y)) * causticsWaveAmp;
    float2 wp       = worldPos.xz + waveDisp;

    // WHAT: XZ 平面に投影した UV を時間でずらし、2 つのスケールを重ねて水中の光模様を作る。
    // WHY: HDR 色を読み返さず加算合成だけで成立させるため、PS はコースティクス寄与色のみを出力する。
    float tiling = customParameters.y;
    float timeOffset = customParameters.w;
    float2 uv0 = wp * tiling + float2(timeOffset, timeOffset * 0.73f);
    float2 uv1 = wp * tiling * 1.37f + float2(-timeOffset * 0.61f, timeOffset * 0.41f);
    float caustics0 = g_causticsTex.Sample(sampTiling, uv0).r;
    float caustics1 = g_causticsTex.Sample(sampTiling, uv1).r;
    float caustics = saturate((caustics0 + caustics1) * 0.5f);

    float attenuation = exp(-waterDepth * 0.45f);
    float intensity = caustics * attenuation * customParameters.x * boundsFade;
    float3 color = float3(0.55f, 0.85f, 1.0f) * intensity;
    return float4(color, 1.0f);
}
