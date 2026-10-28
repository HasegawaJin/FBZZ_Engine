/// @file    ManyLightProbe.hlsl
/// @brief   Actual clustered culling and shared PBR light iteration expose HDR energy and evaluation count.
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Rendering/Lighting.hlsli"
cbuffer ManyLightProbeConstants : register(b13)
{
    float4 g_probePosition;
    float4 g_probeMaterial;
}
FBZZFullscreenVertex VSMain(uint id : SV_VertexID) { return FBZZMakeFullscreenVertex(id); }
float4 PSMain(FBZZFullscreenVertex input) : SV_Target0
{
    const float3 position = g_probePosition.xyz;
    const float3 N = float3(0.0f, 1.0f, 0.0f);
    const float3 V = normalize(cameraPos - position);
    float3 result = 0.0f;
    uint count, base;
    FBZZ_PunctualRange(input.svPosition.xy, position, count, base);
    FBZZManyLightReceiver receiver = FBZZ_ManyLightReceiver(N);
    if (g_probeMaterial.w > 0.5f)
        receiver = FBZZ_ManyLightPbrReceiver(N, V, g_probeMaterial.zzz, g_probeMaterial.x, g_probeMaterial.y);
    FBZZ_PUNCTUAL_BEGIN_AT_GUIDED(position, position, input.svPosition.xy, N, receiver)
        result += Lighting_PBR_Direct(N, V, ps.L, g_probeMaterial.zzz, g_probeMaterial.x,
            g_probeMaterial.y, ps.color, ps.intensity);
    FBZZ_PUNCTUAL_END
    return float4(result, (float)FBZZ_PunctualEvaluationCount(count));
}
