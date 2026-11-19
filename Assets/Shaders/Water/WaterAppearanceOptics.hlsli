/// @file    WaterAppearanceOptics.hlsli
/// @brief   標準材質の屈折経路、水中判定、光路の吸収と散乱。
/// @author  Hasegawa Jin
/// @date    2026-11-18
#ifndef FBZZ_WATER_APPEARANCE_OPTICS_HLSLI
#define FBZZ_WATER_APPEARANCE_OPTICS_HLSLI

/// @note WaterStandardAppearance.hlsli から読み込む。
struct WaterHorizonRefraction
{
    float2 uv;
    float pathLength;
    float weight;
    float viewDepth;
};

/// @brief Refine the refracted ray against the opaque depth layer with at most two depth reads.
/// @note A foreground hit, screen exit or depth discontinuity falls back to the original pixel.
/// @see https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission PBRT v4 §9.3.4, water IOR and Snell transmission.
/// @see https://www.jcgt.org/published/0003/04/04/ McGuire and Mara (2014), view-space ray tests against screen depth.
WaterHorizonRefraction TraceWaterHorizonRefraction(float2 screenUV, float3 surfaceVS,
    float3 ray, float sceneDepth, float maxShift)
{
    WaterHorizonRefraction hit = {screenUV, 0.0f, 0.0f, 0.0f};
    const float3 rayVS = mul(float4(ray, 0.0f), view).xyz;
    if (ray.y >= -0.001f || rayVS.z <= 0.05f || sceneDepth <= surfaceVS.z + 0.01f || maxShift <= 0.0f)
        return hit;

    const float waterColumn = sceneDepth - surfaceVS.z;
    float candidateDepth = sceneDepth;
    float shift = 0.0f;
    float residual = 0.0f;
    [unroll]
    for (int step = 0; step < 2; ++step) {
        const float path = (candidateDepth - surfaceVS.z) / rayVS.z;
        const float4 clipPosition = mul(float4(surfaceVS + path * rayVS, 1.0f), projection);
        if (clipPosition.w <= 0.0f) return hit;
        const float2 uv = clipPosition.xy / clipPosition.w * float2(0.5f, -0.5f) + 0.5f;
        const float2 offset = abs(uv - screenUV);
        shift = max(offset.x, offset.y);
        if (any(uv <= 0.0f) || any(uv >= 1.0f) || shift >= maxShift) return hit;

        const float raw = g_sceneDepth.SampleLevel(g_waterPointSampler, uv, 0.0f).r;
        if (raw <= 0.0f) return hit;
        const float sampledDepth = LinearizeDepth(raw);
        residual = abs(sampledDepth - candidateDepth);
        if (sampledDepth <= surfaceVS.z + 0.01f || residual > max(0.06f, 0.45f * waterColumn)) return hit;
        hit.uv = uv;
        candidateDepth = sampledDepth;
    }

    const float2 edge = min(hit.uv, 1.0f - hit.uv);
    const float stableDepth = 1.0f - smoothstep(max(0.025f, 0.08f * waterColumn),
        max(0.08f, 0.24f * waterColumn), residual);
    hit.pathLength = (candidateDepth - surfaceVS.z) / rayVS.z;
    hit.weight = stableDepth * smoothstep(0.0f, 0.025f, min(edge.x, edge.y))
        * (1.0f - smoothstep(0.5f * maxShift, maxShift, shift));
    hit.viewDepth = candidateDepth;
    return hit;
}

/// @note The opaque color and depth snapshots share a texel grid. At a depth edge, sample the color of the validated point hit.
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/texture2d-gatherred HLSL GatherRed returns the bilinear footprint.
float3 SampleWaterHorizonRefraction(WaterHorizonRefraction hit, float surfaceDepth, float sceneThickness)
{
    const float4 depth = g_sceneDepth.GatherRed(g_samplerClamp, hit.uv);
    const float nearest = LinearizeDepth(max(max(depth.x, depth.y), max(depth.z, depth.w)));
    const float farthestRaw = min(min(depth.x, depth.y), min(depth.z, depth.w));
    const float farthest = LinearizeDepth(farthestRaw);
    const float tolerance = max(0.06f, 0.24f * sceneThickness);
    if (farthestRaw <= 0.0f || nearest <= surfaceDepth + 0.01f
        || max(hit.viewDepth - nearest, farthest - hit.viewDepth) > tolerance)
        return g_sceneColor.SampleLevel(g_waterPointSampler, hit.uv, 0.0f).rgb;
    return g_sceneColor.SampleLevel(g_samplerClamp, hit.uv, 0.0f).rgb;
}

struct WaterTransmittedSun
{
    float3 direction;
    float3 irradiance;
};

/// @param swellNormal Filtered geometric normal without the short-wave normal map.
/// @note Project flux across the same interface on both sides of the dielectric boundary.
/// @see https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission PBRT v4 §9.3.4–5, Snell transmission and Fresnel reflectance.
WaterTransmittedSun WaterTransmitSurfaceSun(float3 toSun, float3 irradiance, float3 swellNormal)
{
    WaterTransmittedSun sun = {float3(0.0f, -1.0f, 0.0f), float3(0.0f, 0.0f, 0.0f)};
    if (toSun.y <= 0.0f || !any(irradiance > 0.0f)) return sun;

    /// @note Temper the local slope in the mean water column and fade it at sunset where wave occlusion is unresolved.
    const float swellWeight = 0.5f * smoothstep(0.0f, 0.12f, toSun.y);
    const float3 up = float3(0.0f, 1.0f, 0.0f);
    const float3 entryNormal = SafeNormalize(lerp(up, swellNormal, swellWeight), up);
    const float incidentCosine = dot(toSun, entryNormal);
    if (incidentCosine <= 0.0f) return sun;

    const WaterDielectricInterface entry = WaterEvaluateDielectricInterface(-toSun, entryNormal,
        1.0f / WATER_REFRACTIVE_INDEX);
    if (entry.direction.y >= -0.001f) return sun;
    sun.direction = entry.direction;
    sun.irradiance = irradiance * ((1.0f - entry.reflectance) * incidentCosine / -dot(entry.direction, entryNormal));
    return sun;
}

/// @note Depth grows along the viewing ray. Follow the same ray for the scattering phase and the light path.
float3 WaterEvaluateColumnRadiance(WaterOpticalMedium medium, float pathLength, float3 intoWater,
    float3 bottomRadiance, float3 diffuseSource, WaterTransmittedSun sun, float phaseG)
{
    const WaterOpticalTransport viewTransport = WaterEvaluatePreparedTransport(medium, pathLength);
    float3 radiance = bottomRadiance * viewTransport.transmittance + diffuseSource * viewTransport.singleScatterWeight;
    [branch]
    if (any(sun.irradiance > 0.0f)) {
        const float phase = WaterHenyeyGreensteinPhase(dot(sun.direction, -intoWater), phaseG);
        const float lightTravelPerViewMeter = max(-intoWater.y, 0.0f) / -sun.direction.y;
        radiance += sun.irradiance * phase * WaterIntegrateDirectionalScatter(medium, pathLength, lightTravelPerViewMeter);
    }
    return radiance;
}

/// @note The first interface on a perspective ray uses the camera's medium. Mesh facing can change within a single crest.
bool WaterHorizonCameraUnderwater(float baseWorldY, bool faceUnderwater)
{
    if (isOrthographic > 0.5f) return faceUnderwater;
    const float relativeY = cameraPos.y - baseWorldY;
    if (g_waveShapeParams.y < 0.5f && abs(relativeY) > WaterMaximumWaveRise() + kRippleHeightScale)
        return relativeY < 0.0f;

    const uint owner = (uint)round(abs(g_farOceanParams.w));
    FBZZWaterMediumQueryContext context;
    if (owner == 0u || !FBZZ_WaterMediumHeader(context) || owner > context.bodyCount) return faceUnderwater;
    float4 column;
    if (!FBZZ_WaterAtlasSample(owner - 1u, cameraPos.xz, context, column)) return faceUnderwater;
    const uint row = FBZZ_WATER_MEDIUM_HEADER_ROWS + (owner - 1u) * FBZZ_WATER_MEDIUM_BODY_ROWS;
    const float cameraDepth = FBZZ_WaterMediumRow(row + 6u).x + column.x - cameraPos.y;
    return cameraDepth > 0.0f && cameraDepth <= column.y
        && (g_farOceanParams.w < 0.0f || !FBZZ_WaterPointExcluded(owner - 1u, cameraPos, context));
}

/// @brief 平均面から水に入る方向。画面外の屈折や底面を追えない画素の光路に使う。
/// @see https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission PBRT v4 §9.3.4, Snell's law.
float3 WaterSplineMeanRefraction(float3 toCamera)
{
    const float2 transverse = -toCamera.xz / WATER_REFRACTIVE_INDEX;
    return float3(transverse.x, -sqrt(max(1.0f - dot(transverse, transverse), 0.0f)), transverse.y);
}

struct WaterSplineRefraction
{
    float2 uv;
    float3 direction;
    float pathLength;
    float weight;
};

/// @note Project the bent ray at the visible water-column depth. Deep columns damp the smallest ripples.
/// @see https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission PBRT v4 §9.3.4, Snell transmission.
WaterSplineRefraction WaterTraceSplineRefraction(float2 screenUV, float3 surfaceVS,
    float3 toCamera, float3 swellNormal, float3 surfaceNormal, float2 columnDepths, float strength, float sceneThickness)
{
    WaterSplineRefraction hit = {screenUV, float3(0.0f, -1.0f, 0.0f), 0.0f, 0.0f};
    const float columnDepth = columnDepths.x;
    if (columnDepth <= 0.0f || strength <= 0.0f) return hit;
    const float rippleWeight = lerp(0.82f, 0.42f, smoothstep(0.16f, 2.0f, columnDepth));
    const float3 normal = SafeNormalize(lerp(swellNormal, surfaceNormal, rippleWeight), swellNormal);
    if (dot(normal, toCamera) <= 0.001f) return hit;
    const float3 ray = refract(-toCamera, normal, 1.0f / WATER_REFRACTIVE_INDEX);
    const float3 rayVS = mul(float4(ray, 0.0f), view).xyz;
    if (ray.y >= -0.001f || rayVS.z <= 0.001f) return hit;
    const float3 bottomVS = surfaceVS + rayVS * (columnDepth / -ray.y);
    const float4 clipPosition = mul(float4(bottomVS, 1.0f), projection);
    if (clipPosition.w <= 0.0f) return hit;

    const float2 displacement = NdcToUv(clipPosition.xy / clipPosition.w) - screenUV;
    const float budget = min(strength, 0.10f);
    const float extent = max(abs(displacement.x), abs(displacement.y));
    const float shiftWeight = budget / (budget + extent);
    const float2 shiftedUv = screenUV + displacement * shiftWeight;
    const float2 edge = min(shiftedUv, 1.0f - shiftedUv);
    const float edgeWeight = smoothstep(0.0f, 0.025f, min(edge.x, edge.y));
    const float2 uv = lerp(screenUV, shiftedUv, edgeWeight);
    if (all(uv == screenUV)) return hit;

    /// @note Validate all four texels of the bilinear color footprint to reject sampled depth discontinuities.
    /// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/texture2d-gatherred HLSL GatherRed footprint.
    const float4 depths = g_sceneDepth.GatherRed(g_samplerClamp, uv);
    const float farthestRaw = min(min(depths.x, depths.y), min(depths.z, depths.w));
    if (farthestRaw <= 0.0f) return hit;
    const float nearest = LinearizeDepth(max(max(depths.x, depths.y), max(depths.z, depths.w)));
    const float farthest = LinearizeDepth(farthestRaw);
    const float tolerance = max(0.08f, 0.45f * sceneThickness);
    const float originalBottom = surfaceVS.z + sceneThickness;
    const float mismatch = max(farthest - nearest, max(nearest - originalBottom, originalBottom - farthest));
    if (nearest <= surfaceVS.z + 0.01f || mismatch > tolerance) return hit;

    /// @note Use the accepted bottom depths for color and extinction. A clamped displacement keeps more of the mean path.
    const float sampledDepth = 0.5f * (nearest + farthest);
    const float hitPath = (sampledDepth - surfaceVS.z) / rayVS.z;
    hit.uv = uv;
    hit.direction = ray;
    hit.pathLength = min(hitPath, columnDepths.y / -ray.y);
    hit.weight = shiftWeight * edgeWeight * (1.0f - smoothstep(0.30f * tolerance, tolerance, mismatch));
    return hit;
}

#endif
