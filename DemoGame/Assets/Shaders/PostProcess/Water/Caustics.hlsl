// FBZZ Engine
// PostProcess/Water/Caustics.hlsl | PostProcess
// 深度から復元した水面下のワールド座標へコースティクスを投影する

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Platform/Backend.hlsli"

Texture2D        g_causticsTex : register(TEX_ALBEDO);
Texture2D<float> g_depth       : register(TEX_DEPTH);
SamplerState     sampDefault   : register(SAMPLER_DEFAULT);

struct FSTriVSOut
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

FSTriVSOut VSMain(uint id : SV_VertexID)
{
    FSTriVSOut o;
    o.uv = float2((id & 1u) ? 2.0f : 0.0f,
                  (id & 2u) ? 2.0f : 0.0f);
    o.svPosition = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float4 PSMain(FSTriVSOut p) : SV_Target0
{
    float ndcDepth = g_depth.Sample(sampDefault, p.uv).r;
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

    // WHAT: XZ 平面に投影した UV を時間でずらし、2 つのスケールを重ねて水中の光模様を作る。
    // WHY: HDR 色を読み返さず加算合成だけで成立させるため、PS はコースティクス寄与色のみを出力する。
    float tiling = customParameters.y;
    float timeOffset = customParameters.w;
    float2 uv0 = worldPos.xz * tiling + float2(timeOffset, timeOffset * 0.73f);
    float2 uv1 = worldPos.xz * tiling * 1.37f + float2(-timeOffset * 0.61f, timeOffset * 0.41f);
    float caustics0 = g_causticsTex.Sample(sampDefault, uv0).r;
    float caustics1 = g_causticsTex.Sample(sampDefault, uv1).r;
    float caustics = saturate((caustics0 + caustics1) * 0.5f);

    float attenuation = exp(-waterDepth * 0.45f);
    float intensity = caustics * attenuation * customParameters.x;
    float3 color = float3(0.55f, 0.85f, 1.0f) * intensity;
    return float4(color, 1.0f);
}
