/// @file    SouthForkRiverAppearance.hlsl
/// @brief   Standard river lighting with a short visual handoff to the island's sea.
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include "Water/WaterSurface.hlsli"
#define PSMain ShadeSouthForkWater
#include "Water/WaterStandardAppearance.hlsli"
#undef PSMain

float4 PSMain(WaterPSInput input) : SV_Target0
{
    /// @note Use the unperturbed height so the overlap with Horizontal Sea does not shift with each wave.
    const float coverage = smoothstep(-0.04f, 0.32f, input.baseWorldY);
    float4 shaded = ShadeSouthForkWater(input);
    shaded.a *= coverage;
    clip(shaded.a - 0.003f);
    return shaded;
}
