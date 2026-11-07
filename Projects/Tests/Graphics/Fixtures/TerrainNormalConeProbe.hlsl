/// @file    TerrainNormalConeProbe.hlsl
/// @brief   Reads back the production terrain cone decision for each supplied case.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include "Common/Fullscreen.hlsli"
#include "Terrain/TerrainNormalCone.hlsli"

struct TerrainConeCase
{
    uint packedCone;
    float apexDistance;
    float2 padding;
    float4 center;
    float4 eye;
};

cbuffer TerrainConeProbeConstants : register(b2)
{
    TerrainConeCase g_terrainConeCases[13];
};

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

float4 PSMain(FBZZFullscreenVertex input) : SV_Target0
{
    const TerrainConeCase probe = g_terrainConeCases[min((uint)input.svPosition.x, 12u)];
    const float rejected = IsTerrainMeshletBackFacing(probe.packedCone, probe.apexDistance, probe.center.xyz, probe.eye) ? 1.0f : 0.0f;
    return float4(rejected, 0.0f, 0.0f, 1.0f);
}
