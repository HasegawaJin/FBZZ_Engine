/// @file    CloudUpscale.hlsl
/// @brief   半解像度の雲を拡大して合成する。
/// @author  Hasegawa Jin
/// @date    2026-07-01
/// @note バイリニアでアップスケールして ALPHA_BLEND 合成する。
///
/// @note 雲はレイマーチが重く低周波なので 1/2 解像度で描けば描画ピクセルが 1/4 になる。
/// @note 合成側はサンプル 1 回のみで安価。scatter/alpha は雲なし画素で共に 0 のため、
/// @note バイリニア補間しても比率が保たれ、元のフル解像度合成と同じ見た目になる。
#include "Common/Binding.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D_T(float4, g_cloudHalf, SRV_CLOUD_HALF_SLOT);
SamplerState      sampLinear  : register(SAMPLER_LINEAR_CLAMP); /// @note 全画面フェッチ (s0 は DX12 では WRAP)

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    return g_cloudHalf.Sample(sampLinear, p.uv);
}
