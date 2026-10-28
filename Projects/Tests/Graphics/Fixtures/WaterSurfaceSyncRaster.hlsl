/// @file    WaterSurfaceSyncRaster.hlsl
/// @brief   Capture height and geometric slope after the production WaterRenderPass mesh and LOD.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include "Water/WaterSurface.hlsli"

cbuffer WaterSurfaceSyncRasterOptions : register(b2)
{
    float4 g_syncRasterOptions;
};

/// @note Alpha one preserves numeric RGB through the production alpha blend state. Detail normals are excluded.
float4 PSMain(WaterPSInput input) : SV_Target0
{
    /// @note Capture the actual submitted path and interpolated vertex cell widths, rather than predicting LOD in the test.
    if (g_syncRasterOptions.x > 0.5f)
        return float4(g_farOceanParams.y, input.meshCellSize, 1.0f);
    return float4(input.relativeSurfaceY, normalize(input.normal).xz, 1.0f);
}
