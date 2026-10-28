/// @file    WaterMediumProbe.hlsl
/// @brief   Water medium lookup test shader that writes probe results into an HDR target.
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include "Common/Constants.hlsli"
#include "Rendering/Lighting.hlsli"

cbuffer WaterMediumProbeConstants : register(b13)
{
    float4 g_probePointMode;
    float4 g_probeSource;
};

struct WaterMediumProbeVertex
{
    float4 position : SV_POSITION;
};

WaterMediumProbeVertex VSMain(uint vertexId : SV_VertexID)
{
    WaterMediumProbeVertex output;
    const float2 corner = float2((vertexId & 1u) ? 2.0f : 0.0f,
                                 (vertexId & 2u) ? 2.0f : 0.0f);
    output.position = float4(corner * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float4 PSMain(WaterMediumProbeVertex input) : SV_Target0
{
    if (g_probePointMode.w > 7.5f)
    {
        FBZZWaterMediumQueryContext context;
        const bool valid = FBZZ_WaterMediumHeader(context);
        return float4(valid ? 1.0f : 0.0f, (float)context.bodyCount,
                      (float)context.tableRows, (float)context.atlasWidth);
    }
    if (g_probePointMode.w > 6.5f)
    {
        FBZZWaterMediumQueryContext context;
        float4 atlasValue = 0.0f;
        const bool valid = FBZZ_WaterMediumHeader(context)
            && context.bodyCount > 0u
            && FBZZ_WaterAtlasSample(0u, g_probePointMode.xz, context, atlasValue);
        return float4(atlasValue.rgb, valid ? atlasValue.a : 0.0f);
    }
    if (g_probePointMode.w > 5.5f)
    {
        FBZZWaterMediumSample medium;
        if (!FBZZ_QueryWaterMedium(g_probePointMode.xyz, medium)) return 0.0f;
        return float4(medium.plumeScattering, float(medium.ownerIndex + 1u));
    }
    if (g_probePointMode.w > 3.5f)
    {
        const float3 receiverLighting = Lighting_PBR(g_probePointMode.xyz,
            float3(0.0f, 1.0f, 0.0f), float3(0.0f, 1.0f, 0.0f),
            float3(0.0f, 1.0f, 0.0f), float3(0.8f, 0.8f, 0.8f),
            0.0f, 0.5f, lightColor, lightIntensity, 1.0f, 1.0f);
        const float3 emission = g_probePointMode.w > 4.5f ? g_probeSource.rgb : 0.0f;
        return float4(receiverLighting + emission, 1.0f);
    }

    if (g_probePointMode.w > 2.5f)
    {
        const float ownerTag = g_probePointMode.x > 1.5f
            ? floor(input.position.x + 0.5f)
            : floor(g_probeSource.w + 0.5f);
        return float4(g_probeSource.xyz, ownerTag);
    }

    FBZZWaterMediumSample sample;
    const bool covered = FBZZ_QueryWaterMedium(g_probePointMode.xyz, sample);
    if (!covered)
        return 0.0f;

    if (g_probePointMode.w > 1.5f)
        return float4(FBZZ_WaterCompression(sample), sample.jacobian,
                      sample.absorption.r, (float)sample.ownerIndex + 1.0f);

    if (g_probePointMode.w > 0.5f)
        return float4(FBZZ_WaterSourceRGBTransmittance(g_probePointMode.xyz,
                          g_probeSource.xyz, g_probeSource.w, sample.ownerIndex), 1.0f);

    return float4(sample.surfaceY, sample.depth, sample.jacobian,
                  (float)sample.ownerIndex + 1.0f);
}
