/// @file    SkyCloudCapture.hlsl
/// @brief   空 IBL のキューブ各面へ主ビューと同じ雲を合成する。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include "Rendering/CloudIntegration.hlsli"
#include "Common/Space.hlsli"
#include "Common/Fullscreen.hlsli"

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    float3 farPos = ReconstructWorldPos(p.uv, 0.0f, invViewProjection);
    float3 rd = normalize(farPos - cameraPos);
    /// @note 捕捉は深度を持たない空だけ。固定の中点標本でキャッシュ更新時の明滅を抑える。
    return FBZZIntegrateCloud(cameraPos, rd, cloudNoise.w, 0.5f, 32);
}
