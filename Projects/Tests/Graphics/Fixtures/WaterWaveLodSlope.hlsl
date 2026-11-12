/// @file    WaterWaveLodSlope.hlsl
/// @brief   Capture the combined geometric and residual wave slope across a Water LOD boundary.
/// @author  Hasegawa Jin
/// @date    2026-11-06
#include "Water/WaterSurface.hlsli"

float4 PSMain(WaterPSInput input) : SV_Target0
{
    float lostVariance;
    float2 residual = WaterResidualWaveSlope(input.baseWorldXZ, g_timeParams.w,
        ddx(input.baseWorldXZ), ddy(input.baseWorldXZ), input.waveMeshFade, lostVariance);
    float2 geometricSlope = -input.normal.xz / input.normal.y;
    return float4(geometricSlope.x + residual.x, input.baseWorldXZ, 1.0f);
}
