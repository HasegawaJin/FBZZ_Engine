/// @file    SSRResolve.hlsl
/// @brief   不透明 HDR へ環境鏡面の SSR 置換差分を加算する。
/// @author  Hasegawa Jin
/// @date    2026-10-09
/// @note ADDITIVE の RGB は符号付き。直接光・拡散 GI・発光と出力 alpha は保持する。
#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/BindlessIndices.hlsli"
FBZZ_TEX2D(texReflectionDelta, TEX_SSR_SLOT);
SamplerState sampPoint : register(SAMPLER_POINT_CLAMP);
FBZZFullscreenVertex VSMain(uint id : SV_VertexID) { return FBZZMakeFullscreenVertex(id); }
float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    const float3 delta = texReflectionDelta.SampleLevel(sampPoint, p.uv, 0.0f).rgb;
    if (!all(isfinite(delta)) || !isfinite(ssrIntensity) || ssrIntensity <= 0.0f) return 0.0f;
    const float3 weightedDelta = delta * saturate(ssrIntensity);
    /// @note miss と強度0の画素は alpha も素通しにし、透明なカメラ背景を不透明へ変えない。
    if (all(weightedDelta == 0.0f)) return 0.0f;
    /// @note ADDITIVE は src.rgb * src.a を加える。alpha=1 で差分を適用し、不透明 HDR の alpha=1 を保つ。
    return float4(weightedDelta, 1.0f);
}
