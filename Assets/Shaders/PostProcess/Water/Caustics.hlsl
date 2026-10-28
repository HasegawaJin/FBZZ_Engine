/// @file    Caustics.hlsl
/// @brief   Directional-light caustics projected through the authored rectangular water surface.
/// @author  Hasegawa Jin
/// @date    2026-06-23

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Rendering/Underwater.hlsli"
#include "Rendering/WaterMedium.hlsli"

FBZZ_TEX2D(g_causticsTex, TEX_ALBEDO_SLOT);
FBZZ_TEX2D_T(float, g_depth, TEX_DEPTH_SLOT);
FBZZ_TEX2D_T(float, texShadow, TEX_SHADOW_SLOT);
FBZZ_TEX2D_T(float4, g_waterInterface, 13);
SamplerState sampTiling : register(SAMPLER_DEFAULT);
SamplerState sampDepth : register(SAMPLER_LINEAR_CLAMP);
SamplerComparisonState sampShadow : register(SAMPLER_SHADOW);

/// @brief Builds the full-screen triangle used by the projection pass.
/// @param id Vertex identifier supplied by the graphics API.
/// @return Full-screen triangle vertex data.
FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

/// @brief Sample the water atlas surface and derive its local macro normal from neighboring atlas texels.
bool FBZZ_WaterSurfaceFrame(uint bodyIndex, float2 worldXZ,
                            FBZZWaterMediumQueryContext context, out float surfaceY,
                            out float3 normal, out float jacobian, out float atlasDepth, out float curvature)
{
    surfaceY = 0.0f;
    normal = float3(0.0f, 1.0f, 0.0f);
    jacobian = 1.0f;
    atlasDepth = 0.0f;
    curvature = 0.0f;
    float4 center;
    if (!FBZZ_WaterAtlasSample(bodyIndex, worldXZ, context, center)) return false;
    const uint bodyOffset = FBZZ_WATER_MEDIUM_HEADER_ROWS + bodyIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
    const float4 limits = FBZZ_WaterMediumRow(bodyOffset + 6u);
    const float4 bounds = FBZZ_WaterMediumRow(bodyOffset + 9u);
    const float4 tile = FBZZ_WaterMediumRow(bodyOffset + 8u);
    const float2 span = max(bounds.zw - bounds.xy, 1.0e-4f);
    const float2 tilePixels = max((tile.zw - tile.xy) * float2(context.atlasWidth, context.atlasHeight), 1.0f);
    /// @note A four-texel stencil averages the piecewise-linear surface capture before deriving focus.
    const float2 texelWorld = span / tilePixels * 4.0f;
    const float2 leftXZ = max(worldXZ - float2(texelWorld.x, 0.0f), bounds.xy);
    const float2 rightXZ = min(worldXZ + float2(texelWorld.x, 0.0f), bounds.zw);
    const float2 downXZ = max(worldXZ - float2(0.0f, texelWorld.y), bounds.xy);
    const float2 upXZ = min(worldXZ + float2(0.0f, texelWorld.y), bounds.zw);
    float4 leftValue = center;
    float4 rightValue = center;
    float4 downValue = center;
    float4 upValue = center;
    float4 neighbor;
    if (FBZZ_WaterAtlasSample(bodyIndex, leftXZ, context, neighbor)) leftValue = neighbor;
    if (FBZZ_WaterAtlasSample(bodyIndex, rightXZ, context, neighbor)) rightValue = neighbor;
    if (FBZZ_WaterAtlasSample(bodyIndex, downXZ, context, neighbor)) downValue = neighbor;
    if (FBZZ_WaterAtlasSample(bodyIndex, upXZ, context, neighbor)) upValue = neighbor;
    const float dx = max(rightXZ.x - leftXZ.x, 1.0e-4f);
    const float dz = max(upXZ.y - downXZ.y, 1.0e-4f);
    const float dYdX = (rightValue.x - leftValue.x) / dx;
    const float dYdZ = (upValue.x - downValue.x) / dz;
    const float leftDistance = max(worldXZ.x - leftXZ.x, 1.0e-4f);
    const float rightDistance = max(rightXZ.x - worldXZ.x, 1.0e-4f);
    const float downDistance = max(worldXZ.y - downXZ.y, 1.0e-4f);
    const float upDistance = max(upXZ.y - worldXZ.y, 1.0e-4f);
    curvature = 2.0f * ((rightValue.x - center.x) / rightDistance
        - (center.x - leftValue.x) / leftDistance) / dx
        + 2.0f * ((upValue.x - center.x) / upDistance
        - (center.x - downValue.x) / downDistance) / dz;
    surfaceY = limits.x + center.x;
    normal = normalize(float3(-dYdX, 1.0f, -dYdZ));
    jacobian = center.z;
    atlasDepth = min(max(center.y, 0.0f), max(limits.z, 0.0f));
    return isfinite(surfaceY) && all(isfinite(normal)) && isfinite(jacobian) && atlasDepth > 0.0f;
}

/// @brief Evaluate caustics for the per-pixel visible water owner captured by the interface pass.
float4 FBZZ_EvaluateWaterMediumCaustics(float3 worldPos, float3 receiverNormal,
                                        uint bodyIndex, FBZZWaterMediumQueryContext context,
                                        float3 worldDx, float3 worldDy)
{
    const uint bodyOffset = FBZZ_WATER_MEDIUM_HEADER_ROWS + bodyIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
    const float4 flags = FBZZ_WaterMediumRow(bodyOffset + 11u);
    if (flags.z < 0.5f) return float4(0.0f, 0.0f, 0.0f, 1.0f);
    FBZZWaterMediumSample medium;
    if (!FBZZ_WaterMediumBodySample(bodyIndex, worldPos, context, true, true, medium))
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    const float3 lightDelta = -lightDir;
    const float lightLength = length(lightDelta);
    if (lightLength <= 1.0e-5f || lightIntensity <= 0.0f || skyDimmer <= 0.0f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    const float3 L = lightDelta / lightLength;
    if (L.y <= 0.0f) return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float3 entry = worldPos;
    float3 waterToSource = float3(0.0f, 1.0f, 0.0f);
    float3 surfaceNormal = float3(0.0f, 1.0f, 0.0f);
    float surfaceY = 0.0f;
    float jacobian = 1.0f;
    float surfaceDepth = 0.0f;
    float curvature = 0.0f;
    float sourceDistance = 0.0f;
    [unroll]
    for (uint iteration = 0u; iteration < 3u; ++iteration)
    {
        if (!FBZZ_WaterSurfaceFrame(bodyIndex, entry.xz, context,
                                    surfaceY, surfaceNormal, jacobian, surfaceDepth, curvature))
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        const float3 incidentWater = refract(-L, surfaceNormal, 1.0f / 1.333f);
        const float refractedLength = length(incidentWater);
        if (refractedLength <= 1.0e-5f) return float4(0.0f, 0.0f, 0.0f, 1.0f);
        waterToSource = -incidentWater / refractedLength;
        if (waterToSource.y <= 1.0e-5f) return float4(0.0f, 0.0f, 0.0f, 1.0f);
        sourceDistance = (surfaceY - worldPos.y) / waterToSource.y;
        if (sourceDistance <= 0.0f || sourceDistance > medium.maxDistance)
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        entry = worldPos + waterToSource * sourceDistance;
    }

    if (!FBZZ_WaterSurfaceFrame(bodyIndex, entry.xz, context,
                                surfaceY, surfaceNormal, jacobian, surfaceDepth, curvature))
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    sourceDistance = (surfaceY - worldPos.y) / max(waterToSource.y, 1.0e-5f);
    if (sourceDistance <= 0.0f || sourceDistance > medium.maxDistance)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    entry = worldPos + waterToSource * sourceDistance;
    if (FBZZ_WaterPointExcluded(bodyIndex, entry - waterToSource * 1.0e-3f, context))
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    const float receiverNdotL = saturate(dot(receiverNormal, waterToSource));
    if (receiverNdotL <= 0.0f) return float4(0.0f, 0.0f, 0.0f, 1.0f);
    const float cosIncident = saturate(dot(surfaceNormal, L));
    const float eta = 1.333f;
    const float sinTransmittedSquared = (1.0f - cosIncident * cosIncident) / (eta * eta);
    const float cosTransmitted = sqrt(saturate(1.0f - sinTransmittedSquared));
    const float rsNumerator = cosIncident - eta * cosTransmitted;
    const float rsDenominator = max(cosIncident + eta * cosTransmitted, 1.0e-5f);
    const float rpNumerator = eta * cosIncident - cosTransmitted;
    const float rpDenominator = max(eta * cosIncident + cosTransmitted, 1.0e-5f);
    const float fresnel = 0.5f * (rsNumerator * rsNumerator / (rsDenominator * rsDenominator)
                                + rpNumerator * rpNumerator / (rpDenominator * rpDenominator));
    /// @note Surface receivers need the same slope bias as ordinary lighting; volume sample positions do not.
    /// @see https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps Depth bias and self shadowing.
    const float receiverOffset = min((length(worldDx) + length(worldDy)) * 0.5f, 0.02f);
    const float visibility = FBZZ_WaterGeometryVisibilityScaled(texShadow, sampShadow,
        worldPos + receiverNormal * receiverOffset,
        ApplySlopeScaledBias(1.0f, receiverNormal, L));
    if (visibility <= 0.001f) return float4(0.0f, 0.0f, 0.0f, 1.0f);

    const float3 sourceBeer = FBZZ_WaterCausticSourceRGBTransmittance(
        worldPos, waterToSource, sourceDistance, bodyIndex);
    const float2 patternPosition = entry.xz + surfaceNormal.xz * (medium.distortion * medium.depth);
    const float timeOffset = customParameters.w;
    const float tiling = max(customParameters.y, 0.0f);
    const float2 uv0 = patternPosition * tiling + float2(timeOffset, timeOffset * 0.73f);
    const float2 uv1 = patternPosition * tiling * 1.37f
                     + float2(-timeOffset * 0.61f, timeOffset * 0.41f);
    /// @note Explicit receiver-plane footprints remain valid after divergent owner and shadow tests.
    /// @see https://www.pbr-book.org/4ed/Textures_and_Materials/Image_Texture Image Texture Evaluation.
    const float2 projectedDx = worldDx.xz - waterToSource.xz * (worldDx.y / max(waterToSource.y, 1.0e-5f));
    const float2 projectedDy = worldDy.xz - waterToSource.xz * (worldDy.y / max(waterToSource.y, 1.0e-5f));
    const float pattern = saturate((g_causticsTex.SampleGrad(sampTiling, uv0,
        projectedDx * tiling, projectedDy * tiling).r
        + g_causticsTex.SampleGrad(sampTiling, uv1,
            projectedDx * tiling * 1.37f, projectedDy * tiling * 1.37f).r) * 0.5f);
    const float focus = clamp(1.0f + max(1.0f - jacobian, 0.0f) * medium.causticsIntensity,
                              0.25f, 4.0f);
    /// @note A flat, uncompressed surface cannot create a moving concentration pattern.
    /// @see https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-2-rendering-water-caustics Wave curvature concentrates refracted light.
    const float waveFocus = saturate(max(1.0f - jacobian, 0.0f) * 3.0f
        + abs(curvature) * min(sourceDistance, 8.0f) * 3.0f);
    const float intensity = pattern * focus * waveFocus * receiverNdotL * (1.0f - saturate(fresnel))
                          * visibility * SampleCloudShadow(worldPos) * medium.causticsIntensity;
    const float3 radiance = lightColor * (max(lightIntensity, 0.0f) * skyDimmer)
                          * sourceBeer * intensity;
    return float4(max(radiance, 0.0f), 1.0f);
}

/// @brief Projects refracted directional caustics onto visible water-bottom geometry.
/// @param p Full-screen triangle interpolation data.
/// @return Additive HDR caustic radiance with opaque blend coverage.
/// @see https://dev.epicgames.com/documentation/unreal-engine/single-layer-water-shading-model-in-unreal-engine Epic Single Layer Water, water absorption and scattering coefficients.
float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    float ndcDepth = g_depth.Sample(sampDepth, p.uv).r;
    if (IsFarDepth(ndcDepth))
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float3 worldPos = ReconstructWorldPos(p.uv, ndcDepth, invViewProjection);
    const float3 worldDx = ddx(worldPos);
    const float3 worldDy = ddy(worldPos);
    if (lightIntensity <= 0.0f || skyDimmer <= 0.0f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float3 receiverNormal = cross(worldDx, worldDy);
    float normalLength = length(receiverNormal);
    receiverNormal = normalLength > 1.0e-6f ? receiverNormal / normalLength : float3(0.0f, 1.0f, 0.0f);
    /// @note Facing by world Y flips near-vertical normals with reconstruction roundoff and produces moving speckled caustics.
    const float3 towardCamera = isOrthographic > 0.5f
        ? -normalize(float3(view[0][2], view[1][2], view[2][2]))
        : cameraPos - worldPos;
    if (dot(receiverNormal, towardCamera) < 0.0f) receiverNormal = -receiverNormal;

    FBZZWaterMediumQueryContext mediumContext;
    if (FBZZ_WaterMediumHeader(mediumContext) && mediumContext.bodyCount > 0u)
    {
        bool interfaceValid = true;
#if BINDLESS_SUPPORTED
        interfaceValid = IsBindlessValid(FbzzPixelSlot(13));
#endif
        if (!interfaceValid) return float4(0.0f, 0.0f, 0.0f, 1.0f);
        uint interfaceWidth = 0u;
        uint interfaceHeight = 0u;
        g_waterInterface.GetDimensions(interfaceWidth, interfaceHeight);
        if (interfaceWidth == 0u || interfaceHeight == 0u)
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        const int2 interfacePixel = clamp((int2)p.svPosition.xy, int2(0, 0),
                                          int2(interfaceWidth, interfaceHeight) - 1);
        const float4 interfaceSample = g_waterInterface.Load(int3(interfacePixel, 0));
        if (interfaceSample.a > 0.5f)
        {
            const uint encodedOwner = (uint)round(interfaceSample.r);
            if (encodedOwner == 0u || encodedOwner > mediumContext.bodyCount)
                return float4(0.0f, 0.0f, 0.0f, 1.0f);
            return FBZZ_EvaluateWaterMediumCaustics(worldPos, receiverNormal,
                                                   encodedOwner - 1u, mediumContext, worldDx, worldDy);
        }

        FBZZWaterMediumSample receiverMedium;
        if (!FBZZ_QueryWaterMediumWithContext(worldPos, mediumContext, true, true, receiverMedium))
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        return FBZZ_EvaluateWaterMediumCaustics(worldPos, receiverNormal,
                                               receiverMedium.ownerIndex, mediumContext, worldDx, worldDy);
    }

    const float waterDepth = underwaterSurfaceY - worldPos.y;
    if (waterDepth <= 0.0f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    float2 halfExtent = float2(causticsHalfExtentX, causticsHalfExtentZ);
    float2 localXZ = FBZZ_UnderwaterWorldToLocalXZ(worldPos.xz, underwaterCenter,
                                                   underwaterCosYaw, underwaterSinYaw);
    float boundsFade = 1.0f;
    if (halfExtent.x > 0.0f && halfExtent.y > 0.0f)
    {
        float2 edgeDistance = halfExtent - abs(localXZ);
        float2 edgeFade = saturate(edgeDistance / max(halfExtent * 0.1f, 1.0e-3f));
        boundsFade = min(edgeFade.x, edgeFade.y);
        if (boundsFade <= 0.0f)
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    float lightLength = length(-lightDir);
    if (lightLength <= 1.0e-5f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    float3 L = -lightDir / lightLength;
    float sunElevation = saturate(L.y);
    if (sunElevation <= 0.0f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float3 geometricNormal = float3(0.0f, 1.0f, 0.0f);
    float3 incidentAir = -L;
    /// @see https://www.graphics.cornell.edu/~bjw/microfacetbsdf.pdf Walter et al. (2007) §4.2, Snell refraction across a dielectric interface.
    float3 incidentWater = refract(incidentAir, geometricNormal, 1.0f / 1.333f);
    float incidentLength = length(incidentWater);
    if (incidentLength <= 1.0e-5f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    incidentWater /= incidentLength;
    float3 waterToSource = -incidentWater;
    float sourceDistance = waterDepth / max(waterToSource.y, 1.0e-5f);
    if (waterToSource.y <= 1.0e-5f || sourceDistance > underwaterMaxDistance)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    float receiverNdotL = saturate(dot(receiverNormal, waterToSource));
    if (receiverNdotL <= 0.0f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float3 surfaceEntry = worldPos + waterToSource * sourceDistance;
    if (!FBZZ_UnderwaterContainsXZ(surfaceEntry.xz, underwaterCenter, halfExtent,
                                   underwaterCosYaw, underwaterSinYaw))
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float fresnel = 0.02f + 0.98f * pow(1.0f - sunElevation, 5.0f);
    float interfaceTransmission = 1.0f - saturate(fresnel);
    float geometryVisibility = FBZZ_WaterGeometryVisibilityScaled(texShadow, sampShadow, worldPos,
        ApplySlopeScaledBias(1.0f, receiverNormal, L));
    if (geometryVisibility <= 0.001f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    float shadow = geometryVisibility * SampleCloudShadow(worldPos);

    float3 absorption = underwaterOpticalMode > 0.5f
        ? underwaterAbsorption : float3(0.45f, 0.45f, 0.45f);
    float3 scattering = underwaterOpticalMode > 0.5f
        ? underwaterScattering : float3(0.0f, 0.0f, 0.0f);
    WaterOpticalTransport waterTransport = WaterEvaluateOpticalTransport(
        absorption, scattering, sourceDistance);

    float2 waveUV = surfaceEntry.xz * causticsWaveFreq + time * causticsWaveSpeed;
    float2 waveDisplacement = float2(sin(waveUV.x + waveUV.y), cos(waveUV.x - waveUV.y)) * causticsWaveAmp;
    float2 patternPosition = surfaceEntry.xz + waveDisplacement;
    float tiling = customParameters.y;
    float timeOffset = customParameters.w;
    float2 uv0 = patternPosition * tiling + float2(timeOffset, timeOffset * 0.73f);
    float2 uv1 = patternPosition * tiling * 1.37f + float2(-timeOffset * 0.61f, timeOffset * 0.41f);
    float pattern = saturate((g_causticsTex.Sample(sampTiling, uv0).r
                            + g_causticsTex.Sample(sampTiling, uv1).r) * 0.5f);

    float3 sunlight = lightColor * (max(lightIntensity, 0.0f) * skyDimmer);
    float intensity = pattern * boundsFade * receiverNdotL * interfaceTransmission
                    * shadow * max(customParameters.x, 0.0f);
    float3 color = sunlight * waterTransport.transmittance * intensity;
    return float4(max(color, 0.0f), 1.0f);
}
