/// @file    WaterSelection.hlsl
/// @brief   水面と同じ頂点変位・外周判定で選択マスクを描く。
/// @author  Hasegawa Jin
/// @date    2026-10-07

/// @note 固定の VSMain / PSMain 入口を維持し、波・格子・流れ・波紋の式を本描画から共有する。
#define PSMain WaterLitPSMain
#include "Water.hlsl"
#undef PSMain

float4 PSSelection(WaterPSInput p) : SV_Target0
{
    if (WaterSurfaceExcluded(p)) discard;
    float footprint = max(length(ddx(p.worldPos.xz)), length(ddy(p.worldPos.xz)));
    float alpha = p.splineSurface.w > 0.5f ? 1.0f
        : WaterEdgeFade(p.uv, g_refractionFlowParams.zw, g_normalParams.xy, footprint);
    clip(alpha - 0.003f);
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
}

float4 PSMain(WaterPSInput p) : SV_Target0
{
    return PSSelection(p);
}
