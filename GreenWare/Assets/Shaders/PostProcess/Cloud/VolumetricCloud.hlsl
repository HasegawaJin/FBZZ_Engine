/// @file    VolumetricCloud.hlsl
/// @brief   深度で終端した雲の視線積分を HDR へ合成する。
/// @author  Hasegawa Jin
/// @date    2026-07-01
#include "Rendering/CloudIntegration.hlsli"
#include "Common/Space.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Platform/Backend.hlsli"

FBZZ_TEX2D_T(float, g_depth, TEX_DEPTH_SLOT);
SamplerState sampDefault : register(SAMPLER_LINEAR_CLAMP);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    /// @note 半解像度の輪郭では最大深度 (Reversed-Z の最も手前) を使い、物体へ雲をはみ出させない。
    const float4 depthQuad = g_depth.GatherRed(sampDefault, p.uv);
    float ndcDepth = max(max(depthQuad.x, depthQuad.y), max(depthQuad.z, depthQuad.w));
    float sceneDepth = IsFarDepth(ndcDepth)
        ? cloudNoise.w
        : distance(cameraPos, ReconstructWorldPos(p.uv, ndcDepth, invViewProjection));
    float3 farPos = ReconstructWorldPos(p.uv, 0.0f, invViewProjection);
    float3 rd = normalize(farPos - cameraPos);
    /// @note 画素内の開始位置をずらし、有限ステップの帯を散らす。
    float dither = frac(sin(dot(p.uv, float2(12.9898f, 78.233f))) * 43758.5453f);
    return FBZZIntegrateCloud(cameraPos, rd, sceneDepth, dither, 96);
}
