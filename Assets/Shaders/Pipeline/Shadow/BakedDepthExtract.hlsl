/// @file    BakedDepthExtract.hlsl
/// @brief   ベイク済み DSV を数値読み戻し用カラー面へ転写する。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include "Common/Fullscreen.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D_T(float, g_bakedDepth, 0);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float4 PSMain(FBZZFullscreenVertex input) : SV_Target0
{
    return float4(g_bakedDepth.Load(int3(int2(input.svPosition.xy), 0)), 0.0f, 0.0f, 1.0f);
}
