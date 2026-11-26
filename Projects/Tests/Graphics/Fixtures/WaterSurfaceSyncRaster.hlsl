/// @file    WaterSurfaceSyncRaster.hlsl
/// @brief   本番の水面高さ、幾何法線、さざ波の勾配を読み戻す。
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include "Water/WaterSurface.hlsli"

cbuffer WaterSurfaceSyncRasterOptions : register(b2)
{
    float4 g_syncRasterOptions;
};

/// @note アルファを 1 にして本番のブレンド状態でも RGB の計測値を保つ。
float4 PSMain(WaterPSInput input) : SV_Target0
{
    // さざ波の移流だけを測る。参照側は移流オフセットを無効にし、CPU が指定した世界座標で標本化する。
    if (g_syncRasterOptions.y > 0.5f) {
        const bool advect = g_syncRasterOptions.y < 1.5f;
        const float2 velocity = input.splineSurface.yz;
        const float2 position = input.baseWorldXZ + g_syncRasterOptions.zw;
        const float2 positionDx = ddx(input.baseWorldXZ);
        const float2 positionDy = ddy(input.baseWorldXZ);
        const float footprint = max(length(positionDx), length(positionDy));
        WaterCurrentAdvection current = (WaterCurrentAdvection)0;
        current.directionWeight = 1.0f;
        if (advect) {
            current = WaterCurrentAt(velocity, g_timeParams.w);
        }
        const float3 detail = WaterDetailGradient(position, normalize(velocity), 0.0f, g_timeParams.w,
            footprint, positionDx, positionDy, true, current);
        return float4(detail, 1.0f);
    }

    // 実際に送信された描画経路と頂点から補間されたセル幅を取得し、描画時に選ばれた LOD を検証する。
    if (g_syncRasterOptions.x > 0.5f) {
        return float4(g_farOceanParams.y, input.meshCellSize, 1.0f);
    }
    return float4(input.relativeSurfaceY, normalize(input.normal).xz, 1.0f);
}
