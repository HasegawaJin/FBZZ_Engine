/// @file    GraphicsQualityProbe.hlsl
/// @brief   本番の間接光評価と履歴初期値を GPU 読み戻しで観測する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Rendering/IBL.hlsli"
cbuffer QualityConstants : register(b2) { uint mode; float value; uint blocked; float reserved; };
FBZZ_TEXCUBE(environmentMap, 17);
FBZZ_TEX2D_T(float4, brdfMap, 18);
FBZZ_SBUFFER_T(float, qualityExposure, 29);
SamplerState sampLinear : register(SAMPLER_LINEAR_CLAMP);
FBZZFullscreenVertex VSMain(uint id : SV_VertexID) { return FBZZMakeFullscreenVertex(id); }
struct Output { float4 color : SV_Target0; float4 depth : SV_Target1; };
Output PSMain(FBZZFullscreenVertex p)
{
    Output result;
    result.depth = float4(value, 0, 0, 1);
    result.color = float4(0.6f, 0.6f, 0.6f, 1);
    if (mode == 1u)
        result.color.rgb = EvaluateSpecularIBL(float3(0,0,-1), float3(0,0,-1), 1.0f, 1.0f, 0.1f,
            1.0f, environmentMap, brdfMap, 0, 1.0f, sampLinear, sampLinear);
    else if (mode == 2u)
        result.color.rgb = EvaluateIBLFromIrradiance(float3(0,0,-1), float3(0,0,-1), 0.5f, 0.0f, 0.5f,
            0.0f, 1.0f, 1.0f, environmentMap, brdfMap, 0, 1.0f, 0.0f, sampLinear, sampLinear);
    else if (mode == 3u)
    {
        float coverage;
        result.color.rgb = FBZZ_ApplyLightProbes(float3(-0.1f,0,0), float3(-1,0,0), sampLinear, 1.0f, coverage);
    }
    else if (mode == 4u) result.color = float4(-0.2f, 0.3f, -0.1f, 0.0f);
    else if (mode == 5u) result.color = 0.0f;
    else if (mode == 6u) result.color = float4(qualityExposure[0].xxx, 1.0f);
    return result;
}
