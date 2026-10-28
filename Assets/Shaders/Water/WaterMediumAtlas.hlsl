/// @file    WaterMediumAtlas.hlsl
/// @brief   Capture shared Water surface height, physical depth and wave Jacobian into the view atlas.
/// @author  Hasegawa Jin
/// @date    2026-10-09
#define PSMain WaterLitPSMain
#define FBZZ_WATER_MEDIUM_CAPTURE 1
#include "Water/Water.hlsl"
#undef FBZZ_WATER_MEDIUM_CAPTURE
#undef PSMain

/// @note x=table owner index plus one; y=maximum physical depth for columns without terrain sampling.
cbuffer WaterMediumCaptureConstants : register(b10)
{
    float4 g_waterMediumCapture;
}

/// @brief Store geometry-relative height to retain sub-meter waves at large world coordinates.
float4 PSMain(WaterPSInput p) : SV_Target0
{
    float depth = max(g_waterMediumCapture.y, 0.0f);
    if (p.splineSurface.w > 0.5f)
    {
        depth = max(p.splineSurface.x, 0.0f);
    }
    else if (g_terrainInfoParams.x > 0.5f)
    {
        const float2 terrain = SampleWaterTerrainDepth(WaterTerrainUvAtWorldXZ(p.worldPos.xz));
        /// @note Terrain is fixed in world space; wave displacement changes column depth, not its bottom.
        if (terrain.x > 1.0e-4f) depth = max(terrain.y + p.worldPos.y - p.baseWorldY, 0.0f);
    }
    return float4(p.relativeSurfaceY, depth, p.waveJacobian, max(g_waterMediumCapture.x, 0.0f));
}
