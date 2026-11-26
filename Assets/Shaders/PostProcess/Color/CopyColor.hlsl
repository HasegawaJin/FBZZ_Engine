/// @file    CopyColor.hlsl
/// @brief   HDR カラーバッファを別 RenderTarget へコピーする
/// @author  Hasegawa Jin
/// @date    2026-06-01

#include "Common/Binding.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texSource, SRV_POST_COLOR_SLOT);
/// @note 全画面フェッチなので clamp 必須 (s0 は DX12 では WRAP)。
SamplerState sampDefault : register(SAMPLER_LINEAR_CLAMP);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    return texSource.Sample(sampDefault, p.uv);
}
