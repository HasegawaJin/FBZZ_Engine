/// @file    WaterSurfaceTerrain.hlsli
/// @brief   地形水深の復号・座標変換・波の浅水減衰。
/// @author  Hasegawa Jin
/// @date    2026-11-18
#ifndef FBZZ_WATER_SURFACE_TERRAIN_HLSLI
#define FBZZ_WATER_SURFACE_TERRAIN_HLSLI

/// @note WaterSurface.hlsli から読み込む。
float2 WaterTerrainUvAtWorldXZ(float2 worldXZ);
float2 SampleWaterTerrainDepth(float2 uv);

/// @return x=底面の被覆フラグ、y=基準面から底までの符号付き水深 [m]。
float2 DecodeWaterTerrainDepth(float4 texel)
{
    const uint flags = (uint)round(texel.a * 255.0f);
    const bool dry = (flags & 2u) != 0u;
    /// @note Horizon の範囲外深度でも、地上の底は既定の外洋深度へ戻さない。
    if ((flags & 1u) == 0u || ((flags & 4u) != 0u && !(dry && all(g_terrainDomain.zw > 0.0f)))) return 0.0f;
    const uint word = (uint)round(texel.g * 255.0f) * 256u + (uint)round(texel.b * 255.0f);
    const float depth = ((float)word / 65535.0f * 2.0f - 1.0f) * g_terrainInfoParams.y;
    return float2(1.0f, dry ? min(depth, 0.0f) : depth);
}

float2 WaterTerrainExtent()
{
    return all(g_terrainDomain.zw > 0.0f) ? g_terrainDomain.zw : g_normalParams.xy;
}

/// @note CPU WaterDepthField と同じflags/GB16bit/texel中心を使い、係数を復号してから補間する。
/// @see https://pubs.usgs.gov/publication/70030520 Stockdon et al. (2006), wave runup and swash elevation.
float2 WaterDepthTexelFactors(float4 texel, float landFadeDepth)
{
    uint flags = (uint)round(texel.a * 255.0f);
    if ((flags & 1u) == 0u) return 1.0f;
    const bool dry = (flags & 2u) != 0u;
    if (dry && landFadeDepth <= 0.0f) return 0.0f;
    uint word = (uint)round(texel.g * 255.0f) * 256u + (uint)round(texel.b * 255.0f);
    float depth = ((float)word / 65535.0f * 2.0f - 1.0f) * g_terrainInfoParams.y;
    if ((flags & 4u) != 0u) return !dry && depth > 0.0f ? 1.0f : 0.0f;
    if (dry) depth = min(depth, 0.0f);
    const float horizontal = smoothstep(0.0f, g_waveDepthParams.x, depth);
    if (landFadeDepth <= 0.0f) return horizontal;
    const float shoreHeightFactor = 0.65f;
    const float vertical = depth < 0.0f
        ? shoreHeightFactor * smoothstep(-landFadeDepth, 0.0f, depth)
        : lerp(shoreHeightFactor, 1.0f, horizontal);
    return float2(horizontal, vertical);
}

struct WaterWaveDepthState
{
    float3 horizontal;
    float3 vertical;
    float2 terrainDepth;
};

/// @return 各軸の x は振幅係数、yz は world XZ での解析勾配。
WaterWaveDepthState WaterDepthState(float2 baseWorldXZ, bool readTerrainDepth = false)
{
    WaterWaveDepthState state = {float3(1.0f, 0.0f, 0.0f), float3(1.0f, 0.0f, 0.0f), float2(0.0f, 0.0f)};
    if (g_terrainInfoParams.x < 0.5f) return state;
    if (g_waveDepthParams.x <= 0.0f || g_waveDepthParams.y < 0.5f) {
        if (readTerrainDepth) state.terrainDepth = SampleWaterTerrainDepth(WaterTerrainUvAtWorldXZ(baseWorldXZ));
        return state;
    }
    float2 uv = WaterTerrainUvAtWorldXZ(baseWorldXZ);
    float2 clampedUv = saturate(uv);
    float2 terrainExtent = WaterTerrainExtent();
    float2 outside = (uv - clampedUv) * terrainExtent;
    float outsideDistance = length(outside);
    float outsideWidth = max(g_waveDepthParams.z, 0.001f);
    if (outsideDistance >= outsideWidth) return state;
    int2 dimensions = max((int2)g_terrainInfoParams.zw, int2(1, 1));
    float2 pixel = clampedUv * (float2)dimensions - 0.5f;
    int2 basePixel = (int2)floor(pixel);
    float2 blend = frac(pixel);
    const float landFadeDepth = all(g_terrainDomain.zw > 0.0f)
        ? min(g_terrainInfoParams.y, clamp(0.35f * g_waveDepthParams.x, 0.25f, 2.0f)) : 0.0f;
    const bool decodeDepth = readTerrainDepth && outsideDistance == 0.0f;
    float2 factors[4];
    float2 terrain = 0.0f;
    [unroll]
    for (int z = 0; z < 2; ++z) {
        [unroll]
        for (int x = 0; x < 2; ++x) {
            int2 texel = clamp(basePixel + int2(x, z), int2(0, 0), dimensions - 1);
            const float4 packed = g_foamMask.Load(int3(texel, 0));
            factors[z * 2 + x] = WaterDepthTexelFactors(packed, landFadeDepth);
            if (decodeDepth) {
                const float weight = (x == 0 ? 1.0f - blend.x : blend.x) * (z == 0 ? 1.0f - blend.y : blend.y);
                terrain += weight * DecodeWaterTerrainDepth(packed);
            }
        }
    }
    float2 lower = lerp(factors[0], factors[1], blend.x);
    float2 upper = lerp(factors[2], factors[3], blend.x);
    float2 du = lerp(factors[1] - factors[0], factors[3] - factors[2], blend.y) * dimensions.x;
    float2 dv = lerp(factors[2] - factors[0], factors[3] - factors[1], blend.x) * dimensions.y;
    if (outside.x != 0.0f) du = 0.0f;
    if (outside.y != 0.0f) dv = 0.0f;
    float2 axisU = normalize(mul(float4(1.0f, 0.0f, 0.0f, 0.0f), g_worldMatrix).xz);
    float2 axisV = normalize(mul(float4(0.0f, 0.0f, 1.0f, 0.0f), g_worldMatrix).xz);
    float2 gradientX = du * (axisU.x / terrainExtent.x) + dv * (axisV.x / terrainExtent.y);
    float2 gradientZ = du * (axisU.y / terrainExtent.x) + dv * (axisV.y / terrainExtent.y);
    float2 factor = saturate(lerp(lower, upper, blend.y));
    if (outsideDistance > 0.0f) {
        float t = saturate(outsideDistance / outsideWidth);
        float blendOut = t * t * (3.0f - 2.0f * t);
        const float2 distanceGradient = (axisU * outside.x + axisV * outside.y) / outsideDistance;
        const float2 fadeSlope = (1.0f - factor) * (6.0f * t * (1.0f - t) / outsideWidth);
        gradientX = (1.0f - blendOut) * gradientX + fadeSlope * distanceGradient.x;
        gradientZ = (1.0f - blendOut) * gradientZ + fadeSlope * distanceGradient.y;
        factor = lerp(factor, 1.0f, blendOut);
    }
    state.horizontal = float3(factor.x, gradientX.x, gradientZ.x);
    state.vertical = float3(factor.y, gradientX.y, gradientZ.y);
    state.terrainDepth = float2(terrain.x, terrain.y / max(terrain.x, 1.0e-6f));
    return state;
}

/// @brief 4 テクセルの深度を byte 復号してから補間し、底の被覆率と基準水深を返す。
/// @note R の旧泡マスクと互換を保ち、GB の carry を線形フィルターによる byte 丸めで壊さない。
float2 SampleWaterTerrainDepth(float2 uv)
{
    if (any(uv < 0.0f) || any(uv > 1.0f)) return float2(0.0f, 0.0f);
    int2 dimensions = max((int2)g_terrainInfoParams.zw, int2(1, 1));
    float2 pixel = saturate(uv) * (float2)dimensions - 0.5f;
    int2 basePixel = (int2)floor(pixel);
    float2 blend = frac(pixel);
    float2 terrain = 0.0f;
    [unroll]
    for (int z = 0; z < 2; ++z) {
        [unroll]
        for (int x = 0; x < 2; ++x) {
            int2 texel = clamp(basePixel + int2(x, z), int2(0, 0), dimensions - 1);
            const float weight = (x == 0 ? 1.0f - blend.x : blend.x) * (z == 0 ? 1.0f - blend.y : blend.y);
            terrain += weight * DecodeWaterTerrainDepth(g_foamMask.Load(int3(texel, 0)));
        }
    }
    return float2(terrain.x, terrain.y / max(terrain.x, 1.0e-6f));
}

/// @brief 水平変位後の world XZ を地形情報の基準 UV へ戻す。
/// @note 水域は水平な直交 TRS。負 scale の向きも axis に残し、泡を Gerstner の水平変位に追従させない。
float2 WaterTerrainUvAtWorldXZ(float2 worldXZ)
{
    float2 origin = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), g_worldMatrix).xz;
    float2 axisX = normalize(mul(float4(1.0f, 0.0f, 0.0f, 0.0f), g_worldMatrix).xz);
    float2 axisZ = normalize(mul(float4(0.0f, 0.0f, 1.0f, 0.0f), g_worldMatrix).xz);
    float2 offset = worldXZ - origin;
    float2 terrainCenter = all(g_terrainDomain.zw > 0.0f) ? g_terrainDomain.xy : float2(0.0f, 0.0f);
    return 0.5f + (float2(dot(offset, axisX), dot(offset, axisZ)) - terrainCenter)
        / max(WaterTerrainExtent(), 1.0e-6f);
}

#endif
