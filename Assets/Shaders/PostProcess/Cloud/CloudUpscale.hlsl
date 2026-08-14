// FBZZ Engine
// PostProcess/Cloud/CloudUpscale.hlsl | PostProcess
// ハーフ解像度で描いたボリューメトリック雲 (scatter.rgb + alpha) をフル解像度 HDR へ
// バイリニアでアップスケールして ALPHA_BLEND 合成する。
//
// WHY: 雲はレイマーチが重く低周波なので 1/2 解像度で描けば描画ピクセルが 1/4 になる。
//      合成側はサンプル 1 回のみで安価。scatter/alpha は雲なし画素で共に 0 のため、
//      バイリニア補間しても比率が保たれ、元のフル解像度合成と同じ見た目になる。
#include "Common/Binding.hlsli"
#include "Common/Fullscreen.hlsli"

Texture2D<float4> g_cloudHalf : register(t0);
SamplerState      sampLinear  : register(SAMPLER_DEFAULT); // s0: CLAMP_LINEAR

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    return g_cloudHalf.Sample(sampLinear, p.uv);
}
