/// @file    VolumetricLight.cs.hlsl
/// @brief   半解像度のシャドウ付き体積光の積分。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @note RGB は散乱光、A は代表画素の視空間 Z / farZ (空だけ -1)。合成前に全解像度へ再構成する。
/// @see https://developer.download.nvidia.com/assets/gameworks/papers/Fast_Flexible_Physically-Based_Volumetric_Light_Scattering.pdf Apply Lighting / Composite Results
#include "Rendering/CloudVolume.hlsli"
#include "Common/Space.hlsli"
#include "Common/Math.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Rendering/Underwater.hlsli"
#include "Rendering/WaterLocalLights.hlsli"

FBZZ_TEX2D_T(float, texDepth, 7);
FBZZ_TEX2D_T(float, texShadow, 8);
FBZZ_TEX2D_T(float4, texWaterInterface, 13);
SamplerComparisonState sampShadow : register(s1);
FBZZ_RWTEX2D_T(float4, OutputVolumetric, 4);

/// @brief 単位立体角あたりの Henyey-Greenstein 散乱確率。
/// @pre g の絶対値は 1 未満。
float HG(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = 1.0f + g2 - 2.0f * g * cosTheta;
    return (1.0f - g2) / (4.0f * 3.14159265f * pow(max(denom, 1e-6f), 1.5f));
}

/// @note TAA のないフレームでは時間変化を加えず、固定ディザにする。
float InterleavedGradientNoise(float2 p)
{
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

/// @brief 低解像度の区画で最も手前の深度と、その画素を取る。
/// @note 深度を補間すると壁と背景が混ざるため、Reversed-Z の最大値を選ぶ。奇数の最終区画は端へクランプする。
float VolumetricRepresentativeDepth(uint2 halfPixel, uint2 fullSize, uint2 stride, out uint2 fullPixel)
{
    fullPixel = min(halfPixel * stride, fullSize - 1u);
    float depth = texDepth.Load(int3(fullPixel, 0));
    [loop]
    for (uint y = 0u; y < stride.y; ++y)
    {
        [loop]
        for (uint x = 0u; x < stride.x; ++x)
        {
            uint2 candidate = min(halfPixel * stride + uint2(x, y), fullSize - 1u);
            float candidateDepth = texDepth.Load(int3(candidate, 0));
            if (candidateDepth > depth)
            {
                depth = candidateDepth;
                fullPixel = candidate;
            }
        }
    }
    return depth;
}

/// @brief Integrates directional single scattering over the camera ray's water-only segment.
/// @param origin World-space camera-plane ray origin.
/// @param rayDir Normalized world-space camera ray direction.
/// @param endpointDistance Distance from origin to the representative scene sample.
/// @param fullPixel Full-resolution pixel used for stable spatial jitter.
/// @param taaOffsetPx Current TAA jitter in full-resolution pixels.
/// @return HDR water-volume radiance; view transmittance is applied later in Composite.
/// @note Each view cell integrates homogeneous Beer attenuation and scattering exactly; jitter samples local lighting terms.
/// @see https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance PBRT v4, §11.2 and §11.3; separate light and view transmittance.
float3 FBZZ_IntegrateUnderwaterLight(float3 origin, float3 rayDir, float endpointDistance,
                                    uint2 fullPixel, float2 taaOffsetPx)
{
    if (underwaterLightStrength <= 0.0f || underwaterMaxDistance <= 0.0f
        || lightIntensity <= 0.0f || skyDimmer <= 0.0f)
        return 0.0f;

    float rayStart;
    float rayEnd;
    if (!FBZZ_ClipUnderwaterRay(origin, rayDir, min(endpointDistance, underwaterMaxDistance),
            underwaterSurfaceY, underwaterBottomY, underwaterCenter, underwaterHalfExtent,
            underwaterCosYaw, underwaterSinYaw, rayStart, rayEnd))
        return 0.0f;

    float3 lightAxis = -lightDir;
    float lightLength = length(lightAxis);
    if (lightLength <= 1.0e-5f) return 0.0f;
    float3 L = lightAxis / lightLength;
    if (L.y <= 1.0e-4f) return 0.0f;

    /// @note Directional light travels from air into water; the reverse direction points from a sample toward its refracted source.
    float3 incidentAir = -L;
    float3 incidentWater = refract(incidentAir, float3(0.0f, 1.0f, 0.0f), 1.0f / 1.333f);
    float waterLightLength = length(incidentWater);
    if (waterLightLength <= 1.0e-5f) return 0.0f;
    float3 waterToSource = -incidentWater / waterLightLength;
    if (waterToSource.y <= 1.0e-4f) return 0.0f;

    float cosIncident = saturate(L.y);
    float fresnel = 0.02f + 0.98f * pow(1.0f - cosIncident, 5.0f);
    float surfaceTransmission = 1.0f - saturate(fresnel);
    float phaseG = WaterOpticalClampPhaseG(underwaterPhaseG);
    int steps = clamp((int)round(underwaterLightSteps), 16, 64);
    float stepDistance = (rayEnd - rayStart) / float(steps);
    if (stepDistance <= 1.0e-5f) return 0.0f;

    float temporalPhase = frac(taaOffsetPx.x * 1.61803399f + taaOffsetPx.y * 2.41421356f);
    float jitter = frac(InterleavedGradientNoise(float2(fullPixel)) + temporalPhase);
    float3 sigmaA = underwaterAbsorption;
    float3 sigmaS = underwaterScattering;
    float3 sigmaT = float3(
        WaterOpticalSanitizeCoefficient(sigmaA.x) + WaterOpticalSanitizeCoefficient(sigmaS.x),
        WaterOpticalSanitizeCoefficient(sigmaA.y) + WaterOpticalSanitizeCoefficient(sigmaS.y),
        WaterOpticalSanitizeCoefficient(sigmaA.z) + WaterOpticalSanitizeCoefficient(sigmaS.z));
    sigmaS = float3(WaterOpticalSanitizeCoefficient(sigmaS.x),
                    WaterOpticalSanitizeCoefficient(sigmaS.y),
                    WaterOpticalSanitizeCoefficient(sigmaS.z));
    WaterOpticalTransport viewCellTransport = WaterEvaluateOpticalTransport(sigmaA, sigmaS, stepDistance);
    /// @note 位相は単位立体角で正規化済みなので、表面と同じアーティスト強度を放射照度へπ倍する。
    /// @see Assets/Shaders/Rendering/Lighting.hlsli LIGHT_UNIT_SCALEの単位契約。
    float3 lightIrradiance = lightColor * (max(lightIntensity, 0.0f) * skyDimmer * PI);
    float3 accumulated = 0.0f;
    float3 V = -rayDir;
    float2 cloudWindOffset = FBZZCloudWindOffset();

    [loop]
    for (int step = 0; step < steps; ++step)
    {
        float sampleT = min(rayStart + (float(step) + jitter) * stepDistance, rayEnd);
        float3 samplePos = origin + rayDir * sampleT;
        float sampleDepth = max(underwaterSurfaceY - samplePos.y, 0.0f);

        /// @note Sunlight contributes only when its refracted path enters through the top of this authored water rectangle.
        float sourceDistance = (underwaterSurfaceY - samplePos.y) / waterToSource.y;
        if (sourceDistance < 0.0f || sourceDistance > underwaterMaxDistance) continue;
        float3 surfaceEntry = samplePos + waterToSource * sourceDistance;
        if (!FBZZ_UnderwaterContainsXZ(surfaceEntry.xz, underwaterCenter, underwaterHalfExtent,
                underwaterCosYaw, underwaterSinYaw))
            continue;
        WaterOpticalTransport sourceTransport = WaterEvaluateOpticalTransport(sigmaA, sigmaS, sourceDistance);
        float shadow = FBZZ_WaterGeometryVisibility(texShadow, sampShadow, samplePos);
        if (shadow <= 0.001f) continue;
        /// @note 空気側の雲と界面の集光を同じ入射位置から評価し、光の模様を屈折方向へ伸ばす。
        shadow *= FBZZCloudShaftTransmittance(surfaceEntry, L, cloudWindOffset);

        float phase = WaterHenyeyGreensteinPhase(dot(-waterToSource, V), phaseG);
        float focus = FBZZ_UnderwaterWaveFocus(surfaceEntry.xz, sampleDepth,
            underwaterWaveAmplitude, underwaterWaveFrequency, underwaterWaveSpeed, time);
        float cellStartDistance = float(step) * stepDistance;
        float3 viewTransmittance = exp(-min(sigmaT * cellStartDistance, WATER_OPTICAL_MAX_DEPTH));
        float3 incident = lightIrradiance * surfaceTransmission * sourceTransport.transmittance * shadow;
        accumulated += viewTransmittance * incident * phase * viewCellTransport.singleScatterWeight * focus;
    }

    return accumulated * max(underwaterLightStrength, 0.0f);
}

/// @brief Clips one view-ray cell to the resolved body depth and rectangle footprint.
/// @param bodyIndex Body row selected by the shared query.
/// @param medium Resolved local surface and depth at the sample point.
/// @param origin World-space camera-plane ray origin.
/// @param rayDir Normalized world-space camera ray direction.
/// @param cellStart First ray distance of the current integration cell.
/// @param cellEnd Last ray distance of the current integration cell.
/// @param context Validated view-local water table dimensions.
/// @param clippedStart Receives the first in-body distance.
/// @param clippedEnd Receives the last in-body distance.
/// @return True when the cell intersects a nonzero body interval.
/// @note Spline and zone footprints are resolved by the shared point query at the cell sample.
/// @see Assets/Shaders/Rendering/WaterMedium.hlsli FBZZ_WaterClipSlab.
bool FBZZ_ClipSharedWaterCell(uint bodyIndex, FBZZWaterMediumSample medium,
                              float3 origin, float3 rayDir,
                              float cellStart, float cellEnd,
                              FBZZWaterMediumQueryContext context,
                              out float clippedStart, out float clippedEnd)
{
    clippedStart = cellStart;
    clippedEnd = cellEnd;
    const float bottomY = medium.surfaceY - max(medium.depth, 0.0f);
    if (!FBZZ_WaterClipSlab(origin.y, rayDir.y, bottomY, medium.surfaceY,
                            clippedStart, clippedEnd))
        return false;

    const uint bodyOffset = FBZZ_WATER_MEDIUM_HEADER_ROWS + bodyIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
    const float4 flags = FBZZ_WaterMediumRow(bodyOffset + 11u);
    if (flags.w > 0.5f)
    {
        const float2 halfExtent = max(FBZZ_WaterMediumRow(bodyOffset).xy, 0.0f);
        const float4 rowX = FBZZ_WaterMediumRow(bodyOffset + 1u);
        const float4 rowZ = FBZZ_WaterMediumRow(bodyOffset + 3u);
        const float4 origin4 = float4(origin, 1.0f);
        const float localOriginX = dot(rowX, origin4);
        const float localOriginZ = dot(rowZ, origin4);
        const float localDirectionX = dot(rowX.xyz, rayDir);
        const float localDirectionZ = dot(rowZ.xyz, rayDir);
        if (!FBZZ_WaterClipSlab(localOriginX, localDirectionX, -halfExtent.x, halfExtent.x,
                                clippedStart, clippedEnd)
            || !FBZZ_WaterClipSlab(localOriginZ, localDirectionZ, -halfExtent.y, halfExtent.y,
                                   clippedStart, clippedEnd))
            return false;
    }
    return clippedEnd - clippedStart > 1.0e-5f;
}

/// @brief Integrates the current view's shared water media up to opaque depth or its first visible interface.
/// @param origin World-space camera-plane ray origin.
/// @param rayDir Normalized world-space camera ray direction.
/// @param endpointDistance Distance from origin to the representative opaque sample.
/// @param fullPixel Full-resolution pixel used to load the interface and cluster lights.
/// @param taaOffsetPx Current TAA jitter in full-resolution pixels.
/// @param context Validated view-local water table dimensions.
/// @return HDR single-scattering radiance; Composite applies the accumulated view transmittance.
/// @note Dry intervals do not attenuate the ray, so a camera in an exclusion can still reach wet cells.
/// @see Assets/Shaders/Rendering/WaterMedium.hlsli FBZZ_QueryWaterMediumWithContext and finite RGB source transport.
float3 FBZZ_IntegrateSharedWaterLight(float3 origin, float3 rayDir, float endpointDistance,
                                     uint2 fullPixel, float2 taaOffsetPx,
                                     FBZZWaterMediumQueryContext context)
{
    if (underwaterLightStrength <= 0.0f || underwaterMaxDistance <= 0.0f
        || endpointDistance <= 0.0f || context.bodyCount == 0u)
        return 0.0f;

    float rayEnd = min(min(endpointDistance, underwaterMaxDistance), WATER_OPTICAL_MAX_DISTANCE);
    const float4 interfaceValue = texWaterInterface.Load(int3(fullPixel, 0));
    const float interfaceOwnerTag = interfaceValue.x;
    const float interfaceDepthNormalized = interfaceValue.y;
    if (interfaceValue.w > 0.5f && interfaceOwnerTag >= 1.0f
        && interfaceOwnerTag <= (float)context.bodyCount
        && interfaceDepthNormalized > 0.0f && interfaceDepthNormalized <= 1.0001f)
    {
        const float3 viewForward = normalize(float3(view[0][2], view[1][2], view[2][2]));
        const float rayForward = dot(rayDir, viewForward);
        if (rayForward > 1.0e-5f)
        {
            const float interfaceDistance = interfaceDepthNormalized * max(farZ, 1.0e-3f) / rayForward;
            if (interfaceDistance > 0.0f) rayEnd = min(rayEnd, interfaceDistance);
        }
    }
    if (rayEnd <= 1.0e-4f) return 0.0f;

    const int steps = clamp((int)round(underwaterLightSteps), 16, 64);
    const float stepDistance = rayEnd / (float)steps;
    if (stepDistance <= 1.0e-5f) return 0.0f;

    /// @note 共通媒体の保守的な外接範囲を外れるセルは水を含まない。元の積分格子とディザ位相を保って走査だけ省く。
    /// @see Assets/Shaders/Rendering/WaterMedium.hlsli FBZZ_QueryWaterMediumWithContext の媒体境界契約。
    const float4 mediumBounds = FBZZ_WaterMediumRow(1u);
    float waterStart = 0.0f;
    float waterEnd = rayEnd;
    if (!FBZZ_WaterClipSlab(origin.x, rayDir.x, mediumBounds.x, mediumBounds.z, waterStart, waterEnd)
        || !FBZZ_WaterClipSlab(origin.z, rayDir.z, mediumBounds.y, mediumBounds.w, waterStart, waterEnd))
        return 0.0f;
    if (FBZZ_WaterMediumRow(2u).w > 0.5f) {
        const float2 verticalBounds = FBZZ_WaterMediumRow(4u).zw;
        if (!FBZZ_WaterClipSlab(origin.y, rayDir.y, verticalBounds.x, verticalBounds.y, waterStart, waterEnd))
            return 0.0f;
    }
    /// @note 浮動小数点の境界丸めで部分セルを落とさないよう、元の格子で両端を一セルずつ広げる。
    const int firstStep = max(0, (int)floor(waterStart / stepDistance) - 1);
    const int lastStep = min(steps, (int)ceil(waterEnd / stepDistance) + 1);

    const float temporalPhase = frac(taaOffsetPx.x * 1.61803399f + taaOffsetPx.y * 2.41421356f);
    const float jitter = frac(InterleavedGradientNoise(float2(fullPixel)) + temporalPhase);
    const float3 V = -rayDir;
    const bool sunShadowAvailable = FBZZ_WaterShadowResourceAvailable(texShadow, 8u);
    const float lightLength = length(lightDir);
    const float3 L = lightLength > 1.0e-5f ? -lightDir / lightLength : 0.0f;
    const float3 incidentAir = -L;
    const float3 incidentWater = refract(incidentAir, float3(0.0f, 1.0f, 0.0f), 1.0f / 1.333f);
    const float waterSourceLength = length(incidentWater);
    const float3 waterToSource = waterSourceLength > 1.0e-5f ? -incidentWater / waterSourceLength : 0.0f;
    const float cosIncident = saturate(L.y);
    const float fresnel = 0.02f + 0.98f * pow(1.0f - cosIncident, 5.0f);
    const float surfaceTransmission = 1.0f - saturate(fresnel);
    const float3 sunIrradiance = lightColor * (max(lightIntensity, 0.0f) * max(skyDimmer, 0.0f) * PI);
    const float2 clusterPixelCenter = float2(fullPixel) + 0.5f;
    const float2 cloudWindOffset = FBZZCloudWindOffset();
    float3 accumulated = 0.0f;
    float3 viewTransmittance = 1.0f;

    [loop]
    for (int step = firstStep; step < lastStep; ++step)
    {
        const float cellStart = (float)step * stepDistance;
        const float stepCellEnd = min(cellStart + stepDistance, rayEnd);
        const float sampleDistance = min(((float)step + jitter) * stepDistance, rayEnd);
        const float3 samplePos = origin + rayDir * sampleDistance;
        FBZZWaterMediumSample medium;
        bool hasWetLightingSample = FBZZ_QueryWaterMediumWithContext(samplePos, context, true, medium)
            && sampleDistance <= medium.maxDistance;
        float3 lightingPosition = samplePos;
        float lightingDistance = sampleDistance;
        float clippedStart = cellStart;
        float clippedEnd = stepCellEnd;
        float wetDistance = 0.0f;
        if (hasWetLightingSample)
        {
            clippedEnd = min(clippedEnd, medium.maxDistance);
            if (!FBZZ_ClipSharedWaterCell(medium.ownerIndex, medium, origin, rayDir,
                    cellStart, clippedEnd, context, clippedStart, clippedEnd))
                continue;
            wetDistance = FBZZ_WaterUnexcludedDistance(medium.ownerIndex, origin, rayDir,
                clippedStart, clippedEnd, context);
            if (wetDistance <= 1.0e-5f) continue;
        }
        else
        {
            FBZZWaterMediumSample geometryMedium;
            if (!FBZZ_QueryWaterMediumWithContext(samplePos, context, false, geometryMedium)) continue;
            medium = geometryMedium;
            clippedEnd = min(stepCellEnd, medium.maxDistance);
            if (!FBZZ_ClipSharedWaterCell(medium.ownerIndex, medium, origin, rayDir,
                    cellStart, clippedEnd, context, clippedStart, clippedEnd))
                continue;
            wetDistance = FBZZ_WaterUnexcludedDistance(medium.ownerIndex, origin, rayDir,
                clippedStart, clippedEnd, context);
            if (wetDistance <= 1.0e-5f) continue;

            /// @note A partially excluded cell uses a nearby wet point for lighting; a fully dry point contributes extinction only.
            const float probeFractions[4] = { 0.125f, 0.375f, 0.625f, 0.875f };
            [unroll]
            for (uint probe = 0u; probe < 4u && !hasWetLightingSample; ++probe)
            {
                const float candidateDistance = lerp(clippedStart, clippedEnd, probeFractions[probe]);
                FBZZWaterMediumSample candidate;
                if (!FBZZ_QueryWaterMediumWithContext(origin + rayDir * candidateDistance,
                        context, true, candidate) || candidateDistance > candidate.maxDistance)
                    continue;
                const float candidateEnd = min(stepCellEnd, candidate.maxDistance);
                float candidateStart;
                float candidateClippedEnd;
                if (!FBZZ_ClipSharedWaterCell(candidate.ownerIndex, candidate, origin, rayDir,
                        cellStart, candidateEnd, context, candidateStart, candidateClippedEnd))
                    continue;
                const float candidateWetDistance = FBZZ_WaterUnexcludedDistance(candidate.ownerIndex,
                    origin, rayDir, candidateStart, candidateClippedEnd, context);
                if (candidateWetDistance <= 1.0e-5f) continue;
                lightingPosition = origin + rayDir * candidateDistance;
                lightingDistance = candidateDistance;
                medium = candidate;
                clippedStart = candidateStart;
                clippedEnd = candidateClippedEnd;
                wetDistance = candidateWetDistance;
                hasWetLightingSample = true;
            }
        }

        const float distanceBelowSurface = max(medium.surfaceY - lightingPosition.y, 0.0f);
        const float distanceAboveBottom = max(medium.depth - distanceBelowSurface, 0.0f);
        const float distanceToBoundary = min(distanceBelowSurface, distanceAboveBottom);
        const float transition = max(medium.transitionDepth, 1.0e-4f);
        const float lightBoundaryWeight = smoothstep(0.0f, transition, distanceToBoundary);
        const float3 plumeSamplePosition = origin + rayDir * ((cellStart + stepCellEnd) * 0.5f);
        const float3 plumeScattering = FBZZ_WaterPointExcluded(medium.ownerIndex,
            plumeSamplePosition, context) ? 0.0f : medium.plumeScattering;
        const float3 sigmaA = medium.absorption;
        const float3 sigmaS = medium.scattering + plumeScattering;
        const WaterOpticalTransport viewCell = WaterEvaluateOpticalTransport(sigmaA, sigmaS, wetDistance);
        if (hasWetLightingSample && any(viewCell.singleScatterWeight > 0.0f) && medium.lightStrength > 0.0f)
        {
            float3 sourceRadiance = 0.0f;
            if (sunShadowAvailable && lightLength > 1.0e-5f && L.y > 1.0e-4f
                && lightIntensity > 0.0f && skyDimmer > 0.0f
                && waterToSource.y > 1.0e-4f)
            {
                const float sourceDistance = (medium.surfaceY - lightingPosition.y) / waterToSource.y;
                if (sourceDistance > 1.0e-4f && sourceDistance <= medium.maxDistance)
                {
                    const float3 surfaceEntry = lightingPosition + waterToSource * sourceDistance;
                    float visibility = FBZZ_WaterGeometryVisibility(texShadow, sampShadow, lightingPosition);
                    /// @note 完全な影では寄与が零と確定するため、雲と光源側の媒体積分を行わない。
                    if (visibility > 0.0f)
                        visibility *= FBZZCloudShaftTransmittance(surfaceEntry, L, cloudWindOffset);
                    if (visibility > 0.0f) {
                        const float3 sunTransmittance = FBZZ_WaterSourceRGBTransmittance(
                            lightingPosition, waterToSource, sourceDistance, context, medium);
                        const float phase = WaterHenyeyGreensteinPhase(dot(-waterToSource, V), medium.phaseG);
                        sourceRadiance += sunIrradiance * surfaceTransmission * sunTransmittance
                            * (visibility * phase);
                    }
                }
            }

            sourceRadiance += FBZZ_WaterLocalLightSource(lightingPosition, clusterPixelCenter,
                V, context, medium);
            accumulated += viewTransmittance * sourceRadiance * lightBoundaryWeight
                * viewCell.singleScatterWeight * medium.lightStrength;
        }
        viewTransmittance *= viewCell.transmittance;
    }

    return accumulated * max(underwaterLightStrength, 0.0f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 outputSize;
    OutputVolumetric.GetDimensions(outputSize.x, outputSize.y);
    if (any(id.xy >= outputSize)) return;

    uint2 fullSize;
    texDepth.GetDimensions(fullSize.x, fullSize.y);
    uint2 fullPixel;
    const uint2 stride = max((fullSize + outputSize - 1u) / outputSize, 1u);
    float ndcDepth = VolumetricRepresentativeDepth(id.xy, fullSize, stride, fullPixel);
    /// @note Reversed-Z を半精度へ直接保存すると遠方が 0 へ丸まるため、線形深度を farZ で正規化する。
    float representativeDepth = IsFarDepth(ndcDepth) ? -1.0f
        : LinearizeDepth(ndcDepth, nearZ, farZ, isOrthographic) / max(farZ, 0.001f);
    float2 uv = (float2(fullPixel) + 0.5f) / float2(fullSize);

    if (underwaterOpticalMode > 0.5f)
    {
        float3 sceneWorldPos = IsFarDepth(ndcDepth)
            ? ReconstructWorldPos(uv, 0.0f, invViewProjection)
            : ReconstructWorldPos(uv, ndcDepth, invViewProjection);
        float3 cameraRayDir;
        float3 cameraRayOrigin;
        if (isOrthographic > 0.5f)
        {
            cameraRayDir = normalize(float3(view[0][2], view[1][2], view[2][2]));
            float3 nearPlanePos = ReconstructWorldPos(uv, 1.0f, invViewProjection);
            cameraRayOrigin = nearPlanePos - cameraRayDir * nearZ;
        }
        else
        {
            cameraRayOrigin = cameraPos;
            float3 rayDelta = sceneWorldPos - cameraRayOrigin;
            cameraRayDir = rayDelta / max(length(rayDelta), 1.0e-6f);
        }
        float endpointDistance = max(dot(sceneWorldPos - cameraRayOrigin, cameraRayDir), 0.0f);
        float2 taaOffsetPx = float2(taaJitterX * screenWidth, taaJitterY * screenHeight) * 0.5f;
        FBZZWaterMediumQueryContext waterContext;
        const bool sharedWaterReady = FBZZ_WaterMediumHeader(waterContext) && waterContext.bodyCount > 0u;
        float3 waterScatter = sharedWaterReady
            ? FBZZ_IntegrateSharedWaterLight(cameraRayOrigin, cameraRayDir, endpointDistance,
                fullPixel, taaOffsetPx, waterContext)
            : FBZZ_IntegrateUnderwaterLight(cameraRayOrigin, cameraRayDir,
                endpointDistance, fullPixel, taaOffsetPx);
        OutputVolumetric[id.xy] = float4(waterScatter, representativeDepth);
        return;
    }

    if (volSteps <= 0 || volMaxDist <= 0.0f || volLightIntensity <= 0.0f)
    {
        OutputVolumetric[id.xy] = float4(0.0f, 0.0f, 0.0f, representativeDepth);
        return;
    }

    float3 worldPos;
    if (IsFarDepth(ndcDepth))
    {
        float3 farPos = ReconstructWorldPos(uv, 0.0f, invViewProjection);
        worldPos = cameraPos + normalize(farPos - cameraPos) * volMaxDist;
    }
    else
    {
        worldPos = ReconstructWorldPos(uv, ndcDepth, invViewProjection);
        float dist = length(worldPos - cameraPos);
        if (dist > volMaxDist)
            worldPos = cameraPos + normalize(worldPos - cameraPos) * volMaxDist;
    }

    /// @note 細い隙間を積分区間が飛び越えないよう、従来と同じ最低 16 サンプルを保つ。
    const int steps = clamp(volSteps, 16, 128);
    const float phaseG = clamp(volScattering, -0.95f, 0.95f);
    float3 rayDir = worldPos - cameraPos;
    float rayLength = length(rayDir);
    rayDir /= max(rayLength, 1e-6f);
    float marchStart = min(max(volMinDist, 0.0f), rayLength);
    float marchLength = rayLength - marchStart;
    if (marchLength <= 1.0e-4f)
    {
        OutputVolumetric[id.xy] = float4(0.0f, 0.0f, 0.0f, representativeDepth);
        return;
    }

    float stepDist = marchLength / float(steps);
    float lightLen = length(lightDir);
    float3 L = lightLen > 1.0e-4f ? -lightDir / lightLen : float3(0.0f, 1.0f, 0.0f);
    float scatter = HG(dot(rayDir, L), phaseG);
    float3 accumulated = 0.0f;
    float transmittance = 1.0f;
    /// @note 位相を TAA と共に変える。TAA 無効時のジッターは 0 なので画面のちらつきを増やさない。
    float2 taaOffsetPx = float2(taaJitterX * screenWidth, taaJitterY * screenHeight) * 0.5f;
    float temporalPhase = frac(taaOffsetPx.x * 1.61803399f + taaOffsetPx.y * 2.41421356f);
    float jitter = frac(InterleavedGradientNoise(float2(fullPixel)) + temporalPhase);
    float3 lightCol = lightColor * skyDimmer * volTint;
    float fadeSpan = saturate(volEdgeFade);
    float2 cloudWindOffset = FBZZCloudWindOffset();

    [loop]
    for (int step = 0; step < steps; ++step)
    {
        float sampleT = min(marchStart + (float(step) + jitter) * stepDist, rayLength);
        float3 samplePos = cameraPos + rayDir * sampleT;
        float density = volHeightFalloff > 1.0e-5f
            ? exp(-max(samplePos.y - volHeightStart, 0.0f) * volHeightFalloff) : 1.0f;
        float distFade = fadeSpan > 1.0e-4f
            ? 1.0f - smoothstep(volMaxDist * (1.0f - fadeSpan), volMaxDist, sampleT) : 1.0f;
        if (density * distFade <= 1.0e-4f) continue;

        float visibility = FBZZ_WaterGeometryVisibility(texShadow, sampShadow, samplePos);
        /// @note ジオメトリの完全な影では、結果に寄与しない雲密度の読み取りを省く。
        if (visibility > 0.001f)
            visibility *= FBZZCloudShaftTransmittance(samplePos, L, cloudWindOffset);

        /// @note 空ドームと同じ skyDimmer 軸で積分し、地表の lightIntensity は重ねない。
        accumulated += transmittance * lightCol * scatter * visibility * density * distFade * stepDist;
        if (volDensity > 0.0f)
            transmittance *= exp(-volDensity * density * stepDist);
    }

    /// @note 強度は積分値へ一度だけ掛ける。深度 A は加算ブレンド係数として使わない。
    OutputVolumetric[id.xy] = float4(accumulated * max(volLightIntensity, 0.0f), representativeDepth);
}
