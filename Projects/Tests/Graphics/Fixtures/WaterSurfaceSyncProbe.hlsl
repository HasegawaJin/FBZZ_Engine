/// @file    WaterSurfaceSyncProbe.hlsl
/// @brief   Read production water vertex position and geometric normal without mesh interpolation.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#define VSMain WaterSyncProductionVSMain
#include "Water/WaterSurface.hlsli"
#undef VSMain

struct ProbeOutput
{
    float4 position : SV_Position;
    nointerpolation float4 value : TEXCOORD0;
};

/// @note Six identical WaterVertex records form one diagnostic texel; rows store position/height then normal.
ProbeOutput VSMain(WaterVSInput input, uint id : SV_VertexID)
{
    const uint texel = id / 6u;
    const uint row = texel / 32u;
    const uint column = texel % 32u;
    const float2 corners[6] = {
        float2(0, 0), float2(0, 1), float2(1, 0),
        float2(1, 0), float2(0, 1), float2(1, 1)
    };
    const WaterPSInput water = EvaluateWaterVertex(input, 0u);
    const float2 pixel = float2(column, row) + corners[id % 6u];
    ProbeOutput output;
    output.position = float4(pixel.x / 32.0f * 2.0f - 1.0f, 1.0f - pixel.y, 0.5f, 1.0f);
    output.value = row == 0u ? float4(water.worldPos.xz, water.relativeSurfaceY, 1.0f)
        : float4(water.normal, 1.0f);
    return output;
}

float4 PSMain(ProbeOutput input) : SV_Target0
{
    return input.value;
}
