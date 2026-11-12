/// @file    WaterCrestSpray.hlsl
/// @brief   Draws airborne droplets from strong crossing crests in world space.
/// @author  Hasegawa Jin
/// @date    2026-11-10
#define VSMain WaterSurfaceVSMain
#include "Water/WaterSurface.hlsli"
#undef VSMain

/// @note The procedural grid width matches WaterRenderPass.cpp. Fade before its edges can enter the view.
static const uint SPRAY_GRID_WIDTH = 21u;
static const float SPRAY_CELL_SIZE = 4.0f;
static const float SPRAY_INTERVAL = 0.82f;

struct WaterSprayInput
{
    float4 svPosition : SV_Position;
    float2 disc : TEXCOORD0;
    nointerpolation float opacity : TEXCOORD1;
    nointerpolation float3 source : TEXCOORD2;
    nointerpolation float3 center : TEXCOORD3;
};

float WaterSprayRandom(uint seed)
{
    seed ^= seed >> 16u;
    seed *= 0x7feb352du;
    seed ^= seed >> 15u;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16u;
    return float(seed >> 8u) * (1.0f / 16777216.0f);
}

/// @note A time bin and world cell identify a launch independently of the camera and previous frames.
/// @see https://matthias-research.github.io/pages/publications/tallCells.pdf Chentanez and Müller, water spray as a separate particle representation.
WaterSprayInput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    WaterSprayInput output = (WaterSprayInput)0;
    output.svPosition = float4(0.0f, 0.0f, 2.0f, 1.0f);
    const int2 cell = int2(floor(cameraPos.xz / SPRAY_CELL_SIZE))
        + int2(instanceId % SPRAY_GRID_WIDTH, instanceId / SPRAY_GRID_WIDTH) - int(SPRAY_GRID_WIDTH / 2u);
    const uint cellSeed = asuint(cell.x) * 0x9e3779b9u ^ asuint(cell.y) * 0x85ebca6bu;
    const float time = g_timeParams.w;
    const float schedule = time / SPRAY_INTERVAL - WaterSprayRandom(cellSeed);
    const float age = frac(schedule) * SPRAY_INTERVAL;
    const uint seed = cellSeed ^ asuint((int)floor(schedule)) * 0xc2b2ae35u;
    const float lifetime = lerp(0.38f, 0.62f, WaterSprayRandom(seed + 1u));
    if (age >= lifetime) return output;

    const float2 jitter = float2(WaterSprayRandom(seed + 2u), WaterSprayRandom(seed + 3u));
    const float2 xz = (float2(cell) + jitter) * SPRAY_CELL_SIZE;
    const float3 base = float3(xz.x, g_reflectParams.z, xz.y);
    const float proximity = 1.0f - smoothstep(27.0f, 39.0f, distance(cameraPos, base));
    if (proximity <= 0.0f) return output;

    const WaterCrestSample crest = WaterWaveCrestAt(base, time - age, max(g_gridParams.y, 0.25f), WaterDepthState(xz), true);
    const float height = crest.displacement.y / max(g_reflectParams.w, 0.01f);
    const float strength = smoothstep(0.24f, 0.46f, crest.compression) * smoothstep(0.32f, 0.52f, height)
        * smoothstep(0.08f, 0.30f, crest.crossingRise) * saturate(g_crestFoamParams.z);
    if (strength <= 0.002f) return output;

    const float3 source = base + crest.displacement;
    if (g_terrainInfoParams.x > 0.5f) {
        const float2 depth = SampleWaterTerrainDepth(WaterTerrainUvAtWorldXZ(source.xz));
        if (depth.x > 0.5f && depth.y + crest.displacement.y <= 0.02f) return output;
    }
    const float angle = 6.2831853f * WaterSprayRandom(seed + 4u);
    const float2 scatter = float2(cos(angle), sin(angle));
    const float2 velocity = g_flowParams.xy * (1.2f + clamp(g_waveDepthParams.w, -2.0f, 2.0f)) + scatter * 1.1f;
    const float rise = age * (lerp(2.8f, 4.1f, WaterSprayRandom(seed + 5u)) - 4.905f * age);
    if (rise <= 0.0f) return output;

    const float3 center = source + float3(velocity.x * age, rise + 0.035f, velocity.y * age);
    const float fade = smoothstep(0.0f, 0.06f, age) * (1.0f - smoothstep(0.48f, 1.0f, age / lifetime));
    const float radius = lerp(0.045f, 0.115f, WaterSprayRandom(seed + 6u));
    const float2 corner = float2(vertexId == 1u ? 3.0f : -1.0f, vertexId == 2u ? 3.0f : -1.0f);
    const float3 right = float3(view[0][0], view[1][0], view[2][0]);
    const float3 up = float3(view[0][1], view[1][1], view[2][1]);
    const float3 position = center + (right * corner.x + up * corner.y) * radius;
    output.svPosition = mul(float4(position, 1.0f), viewProjection);
    output.disc = corner;
    output.opacity = saturate(strength * 1.8f) * proximity * fade;
    output.source = source;
    output.center = center;
    return output;
}

float4 PSMain(WaterSprayInput p) : SV_Target0
{
    const float radiusSquared = dot(p.disc, p.disc);
    clip(1.0f - radiusSquared);
    if (g_farOceanParams.w > 0.5f) {
        FBZZWaterMediumQueryContext context;
        const uint owner = (uint)round(g_farOceanParams.w);
        if (!FBZZ_WaterMediumHeader(context) || owner > context.bodyCount) discard;
        if (FBZZ_WaterPointExcluded(owner - 1u, p.source, context)
            || FBZZ_WaterPointExcluded(owner - 1u, p.center, context)) discard;
    }
    const float3 irradiance = max(ambientColor, 0.0f) + max(lightColor * lightIntensity, 0.0f) * 0.4f;
    const float alpha = p.opacity * (1.0f - smoothstep(0.33f, 1.0f, radiusSquared));
    return float4(saturate(g_foamColorRoughness.rgb) * irradiance, alpha);
}
