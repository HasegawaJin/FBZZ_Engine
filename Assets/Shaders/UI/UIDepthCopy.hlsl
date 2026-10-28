/// @file    UIDepthCopy.hlsl
/// @brief   Scene depth resampling for world-space UI on the final output.
/// @author  Hasegawa Jin
/// @date    2026-10-27
#include "Common/Binding.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(sceneDepth, TEX_DEPTH_SLOT);
SamplerState pointClamp : register(SAMPLER_POINT_CLAMP);
cbuffer UIDepthCopyConstants : register(CB_UI)
{
    float4 depthOptions;
};

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float PSMain(FBZZFullscreenVertex input) : SV_Depth
{
    /// @note Camera HDR uses Reversed-Z; the final viewport/backbuffer can use conventional Z.
    float depth = sceneDepth.SampleLevel(pointClamp, input.uv, 0).r;
    return depthOptions.x > 0.5f ? depth : 1.0f - depth;
}
