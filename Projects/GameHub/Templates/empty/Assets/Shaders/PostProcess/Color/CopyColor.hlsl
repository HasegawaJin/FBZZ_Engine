// FBZZ Engine
// PostProcess/Color/CopyColor.hlsl | PostProcess
// HDR カラーバッファを別 RenderTarget へコピーする

#include "Common/Binding.hlsli"
#include "Common/Fullscreen.hlsli"

Texture2D    texSource   : register(TEX_GBUFFER0);
SamplerState sampDefault : register(SAMPLER_DEFAULT);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    return texSource.Sample(sampDefault, p.uv);
}
