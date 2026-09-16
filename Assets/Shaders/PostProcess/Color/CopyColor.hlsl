// FBZZ Engine
// PostProcess/Color/CopyColor.hlsl | PostProcess
// HDR カラーバッファを別 RenderTarget へコピーする

#include "Common/Binding.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texSource, TEX_GBUFFER0_SLOT);
// 全画面フェッチなので clamp 必須 (s0 は DX12 では WRAP)。
SamplerState sampDefault : register(SAMPLER_LINEAR_CLAMP);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    return texSource.Sample(sampDefault, p.uv);
}
