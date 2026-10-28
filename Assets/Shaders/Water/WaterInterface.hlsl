/// @file    WaterInterface.hlsl
/// @brief   Capture the nearest visible water interface using the shared view atlas.
/// @author  Hasegawa Jin
/// @date    2026-10-09
#define PSMain WaterLitPSMain
#include "Water/Water.hlsl"
#undef PSMain
#include "Rendering/WaterMedium.hlsli"

/// @brief Store owner index plus normalized positive view depth for the first visible interface.
float4 PSMain(WaterPSInput p) : SV_Target0
{
    FBZZWaterMediumQueryContext context;
    if (!FBZZ_WaterMediumHeader(context)) discard;
    const uint encodedOwner = (uint)round(g_farOceanParams.w);
    if (encodedOwner == 0u || encodedOwner > context.bodyCount) discard;
    const uint bodyIndex = encodedOwner - 1u;
    const uint bodyOffset = FBZZ_WATER_MEDIUM_HEADER_ROWS + bodyIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
    const float4 flags = FBZZ_WaterMediumRow(bodyOffset + 11u);
    if (flags.y < 0.5f && flags.z < 0.5f) discard;
    float4 atlasValue;
    if (!FBZZ_WaterAtlasSample(bodyIndex, p.worldPos.xz, context, atlasValue)) discard;
    if (FBZZ_WaterPointExcluded(bodyIndex, p.worldPos, context)) discard;
    uint sceneWidth = 0u;
    uint sceneHeight = 0u;
    g_sceneDepth.GetDimensions(sceneWidth, sceneHeight);
    const int2 pixel = clamp((int2)p.svPosition.xy, int2(0, 0), int2(sceneWidth, sceneHeight) - 1);
    const float opaqueDepth = g_sceneDepth.Load(int3(pixel, 0)).r;
    if (opaqueDepth > p.svPosition.z + 1.0e-5f) discard;
    const float viewDepth = max(mul(float4(p.worldPos, 1.0f), view).z, 0.0f);
    return float4((float)bodyIndex + 1.0f, viewDepth / max(farZ, 1.0e-3f), 0.0f, 1.0f);
}
