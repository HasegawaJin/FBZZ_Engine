/// @file    Composite.hlsl
/// @brief   HDR と Bloom を合成し、トーンマップ・ガンマ補正した LDR を出力する。
/// @author  Hasegawa Jin
/// @date    2026-06-18

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/ToneMap.hlsli"
#include "Rendering/Fog.hlsli"
#include "Rendering/Atmosphere.hlsli"
#include "Rendering/PostProcess.hlsli"
#include "Common/FroxelFogConstants.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Rendering/Underwater.hlsli"
#include "Rendering/WaterMedium.hlsli"

FBZZ_TEX2D(texHDR, TEX_GBUFFER0_SLOT);        /// @note ライティング結果 HDR バッファ
FBZZ_TEX2D(texBloom, TEX_BLOOM_SLOT);
FBZZ_TEX2D_T(float, texDepth, TEX_DEPTH_SLOT);           /// @note 深度 (フォグ計算用)
FBZZ_TEX3D(texLUT, TEX_LUT_COLOR_GRADE_SLOT); /// @note 3D カラーグレーディング LUT (32x32x32 推奨)
/// @note ---- Advanced Graphics ----
/// @note t21 is reused after TAA for the current-view underwater directional scatter result.
FBZZ_TEX2D_T(float4, texUnderwaterVolume, 21);
FBZZ_TEX2D_T(float4, g_waterInterface, 13);
SamplerState sampPointClamp : register(SAMPLER_POINT_CLAMP);
/// @note 全画面フェッチはすべてこれ 1 本で引く。
/// @note s0 (SAMPLER_DEFAULT) を使わないか: DX12 の静的サンプラーでは s0 が WRAP
/// @note (メッシュテクスチャのタイリング用) になっている。色収差やレンズ歪みは uv を
/// @note 画面外へずらすので、s0 で引くと画面端が反対側の端を読み、上下左右がつながる。
SamplerState       sampLinearClamp : register(SAMPLER_LINEAR_CLAMP);

/// @note ---- フロクセル ボリューメトリック フォグ ----
/// @note froxelGridZ == 0 のとき無効。b13 が未束縛なら全ゼロで読まれるので、そのまま素通りする。
FBZZ_TEX3D_T(float4, texFroxelFog, TEX_FROXEL_FOG_SLOT);

/// @note ---- 自動露出 ----
/// @note [0] = 順応済みの平均輝度。ExposureAverage.cs.hlsl が毎フレーム書く。
/// @note autoExposureKey <= 0 のときは束縛されておらず、b5 の exposure をそのまま使う。
FBZZ_SBUFFER_T(float, gAdaptedLuminance, SB_PUNCTUAL_LIGHTS_SLOT); /// @note t29

/// @note ResolveExposure — b5 の手動 exposure と自動露出を 1 か所で合流させる。

/// @note 中間グレーで割るか: 露出とは「平均輝度を中間グレーへ持ってくる倍率」。
/// @note 0.18 は反射率 18% のグレーカード、つまり写真の露出計が基準にしている明るさ。
float ResolveExposure()
{
    if (autoExposureKey <= 0.0f)
        return exposure;

    const float avg = max(gAdaptedLuminance[0], 1e-5f);
    float ev = log2(autoExposureKey / avg) + autoExposureCompensation;
    ev = clamp(ev, autoExposureMinEV, autoExposureMaxEV);
    /// @note 手動 exposure は自動露出の上に乗る倍率として残す。絵作りの最終調整に使える。
    return exposure * exp2(ev);
}

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

/// @brief カメラ深度 (Reversed-Z) を視空間 Z へ戻す。空 (深度 0) は farZ になる。
/// @note 平行投影も Space.hlsli の LinearizeDepth が分岐して扱う。
float LinearDepthFromNdc(float ndcZ)
{
    return LinearizeDepth(ndcZ, nearZ, farZ, isOrthographic);
}

/// @note ApplyFroxelFog — 積分済みボリュームから「加算する光」と「背景の透過率」を取り出す。
/// @note rgb = 視線に沿って散乱してきた光, a = 背景に掛ける透過率
float3 ApplyFroxelFog(float3 hdr, float2 uv, float ndcDepth)
{
    if (froxelGridZ == 0u) return hdr;

    /// @note 深度が最遠 (スカイドーム) のときは、グリッドの最終スライスまで積分した値になる。
    const float viewZ = LinearDepthFromNdc(ndcDepth);

    /// @note Load でなく Sample。スライス間を補間しないと、粗いグリッドの境界がそのまま画面上の縞になる。
    const float4 fog = FBZZ_SampleIntegratedFroxel(texFroxelFog, sampLinearClamp, uv, viewZ);

    return hdr * fog.a + fog.rgb;
}

/// @brief Applies coefficient water absorption and ambient plus current-frame directional in-scatter in HDR.
/// @param hdr Scene radiance after surface effects and before tone mapping.
/// @param uv Distorted screen UV shared by color, depth, and volume lookups.
/// @param ndcDepth Reversed-Z scene depth at uv.
/// @return Radiance after the finite camera-to-scene water segment.
/// @see https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance PBRT v4, §11.2 homogeneous Beer transmittance.
float3 ApplyUnderwaterOpticsHDR(float3 hdr, float2 uv, float ndcDepth)
{
    float3 rayOrigin;
    float3 rayDirection;
    float3 sceneWorldPos = ReconstructWorldPos(uv, ndcDepth, invViewProjection);
    if (isOrthographic > 0.5f)
    {
        rayDirection = normalize(float3(view[0][2], view[1][2], view[2][2]));
        float3 nearPlanePos = ReconstructWorldPos(uv, 1.0f, invViewProjection);
        rayOrigin = nearPlanePos - rayDirection * nearZ;
    }
    else
    {
        rayOrigin = cameraPos;
        float3 rayDelta = sceneWorldPos - rayOrigin;
        rayDirection = rayDelta / max(length(rayDelta), 1.0e-6f);
    }

    float endpointDistance = IsFarDepth(ndcDepth)
        ? max(underwaterMaxDistance, 0.0f)
        : max(dot(sceneWorldPos - rayOrigin, rayDirection), 0.0f);
    float rayStart;
    float rayEnd;
    if (!FBZZ_ClipUnderwaterRay(rayOrigin, rayDirection,
            min(endpointDistance, underwaterMaxDistance), underwaterSurfaceY, underwaterBottomY,
            underwaterCenter, underwaterHalfExtent, underwaterCosYaw, underwaterSinYaw,
            rayStart, rayEnd))
        return hdr;

    float segmentLength = max(rayEnd - rayStart, 0.0f);
    float3 segmentMidpoint = rayOrigin + rayDirection * (rayStart + segmentLength * 0.5f);
    float midpointDepth = max(underwaterSurfaceY - segmentMidpoint.y, 0.0f);
    float3 absorption = float3(WaterOpticalSanitizeCoefficient(underwaterAbsorption.x),
                               WaterOpticalSanitizeCoefficient(underwaterAbsorption.y),
                               WaterOpticalSanitizeCoefficient(underwaterAbsorption.z));
    float3 scattering = float3(WaterOpticalSanitizeCoefficient(underwaterScattering.x),
                               WaterOpticalSanitizeCoefficient(underwaterScattering.y),
                               WaterOpticalSanitizeCoefficient(underwaterScattering.z));
    WaterOpticalTransport viewTransport = WaterEvaluateOpticalTransport(absorption, scattering, segmentLength);
    float3 sigmaT = absorption + scattering;
    float3 ambientEntryTransmittance = exp(-min(sigmaT * midpointDepth, WATER_OPTICAL_MAX_DEPTH));
    float3 ambientScatter = ambientColor * 0.5f * ambientEntryTransmittance
                          * viewTransport.singleScatterWeight;
    float3 directionalScatter = 0.0f;
    [branch]
    if (underwaterLightReady > 0.5f)
        directionalScatter = texUnderwaterVolume.SampleLevel(sampLinearClamp, uv, 0.0f).rgb;

    float3 waterRadiance = hdr * viewTransport.transmittance + ambientScatter + directionalScatter;
    return lerp(hdr, waterRadiance, saturate(underwaterStrength));
}

/// @brief Filter a proven owner's interior in hardware; holes and mixed coverage retain the ownership-aware four-tap path.
/// @see https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_filter Linear texture filtering.
bool SampleCachedWaterColumn(uint owner, float3 world, float4 bounds, float4 tileUv,
                             float4 limits, float displacement,
                             FBZZWaterMediumQueryContext context, out float4 value)
{
    value = 0.0f;
    const float2 worldXZ = world.xz;
    const float2 span = bounds.zw - bounds.xy;
    if (any(worldXZ < bounds.xy) || any(worldXZ > bounds.zw) || any(span <= 1.0e-5f)) return false;
    const float2 size = float2(context.atlasWidth, context.atlasHeight);
    const int2 first = (int2)floor(tileUv.xy * size);
    const int2 last = max(first, (int2)ceil(tileUv.zw * size) - 1);
    const float2 local = saturate((worldXZ - bounds.xy) / span);
    const float2 pixel = clamp((tileUv.xy + local * (tileUv.zw - tileUv.xy)) * size - 0.5f,
                              (float2)first, (float2)last);
    const float tag = (float)(owner + 1u);
    value = g_waterMediumAtlas.SampleLevel(sampLinearClamp, (pixel + 0.5f) / size, 0.0f);
    /// @note Hardware interpolation has finite fractional precision; retain exact ownership filtering near wave/bottom classification boundaries.
    const float depth = limits.x + value.x - world.y;
    const float physicalDepth = min(max(value.y, 0.0f), max(limits.z, 0.0f));
    const float guard = max(0.01f, displacement * 0.0625f);
    /// @note A producer-owned tile contains only this tag or zero. A full bilinear tag proves its nearest tap is owned; mixed coverage still checks that tap before fallback.
    if (value.a == tag && depth > guard && depth < physicalDepth - guard) return true;
    const int2 nearest = clamp((int2)floor(pixel + 0.5f), first, last);
    if ((uint)round(g_waterMediumAtlas.Load(int3(nearest, 0)).a) != owner + 1u) return false;
    return FBZZ_WaterAtlasSample(owner, worldXZ, context, value);
}

#if !defined(FBZZ_WATER_CACHED_FAST)
/// @brief Query the 16x16 conservative view mask with the same UV used for depth.
bool WaterOpticalTileIsDry(float2 uv)
{
    if (!all(uv >= 0.0f) || !all(uv <= 1.0f)) return false;
    const uint2 tile = min((uint2)(uv * 16.0f), 15u);
    const uint rows = waterOpticalDryTiles[tile.y >> 3u][(tile.y >> 1u) & 3u];
    return (rows & (1u << (tile.x + 16u * (tile.y & 1u)))) != 0u;
}
#endif

/// @brief Integrates RGB water extinction from the camera to the first visible interface or opaque point.
/// @param hdr Scene radiance after surface effects and before tone mapping.
/// @param uv Distorted screen UV shared by color, depth, and interface lookups.
/// @param ndcDepth Reversed-Z scene depth at uv.
/// @return HDR radiance after the current view's per-pixel water path.
/// @see Docs/design/water-gameplay-visuals.md §水域情報; https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance PBRT v4, §11.2.
float3 ApplySharedWaterOpticsHDR(float3 hdr, float2 uv, float ndcDepth)
{
#if !defined(FBZZ_WATER_CACHED_FAST)
    [branch]
    if (underwaterOpticalMode == 2.25f && WaterOpticalTileIsDry(uv)) return hdr;
#endif
#if BINDLESS_SUPPORTED
    if (!IsBindlessValid(FbzzPixelSlot(13))) return hdr;
#endif
    FBZZWaterMediumQueryContext context;
    if (!FBZZ_WaterMediumHeader(context)) return hdr;

    const float3 sceneWorldPos = ReconstructWorldPos(uv, ndcDepth, invViewProjection);
    float3 rayDirection;
    float3 rayOrigin;
    if (isOrthographic > 0.5f)
    {
        rayDirection = normalize(float3(view[0][2], view[1][2], view[2][2]));
        const float3 nearPlanePos = ReconstructWorldPos(uv, 1.0f, invViewProjection);
        rayOrigin = nearPlanePos - rayDirection * nearZ;
    }
    else
    {
        rayOrigin = cameraPos;
        rayDirection = normalize(sceneWorldPos - rayOrigin);
    }
    if (dot(rayDirection, rayDirection) <= 1.0e-8f) return hdr;

    float maxDistance = min(max(underwaterMaxDistance, 0.0f), WATER_OPTICAL_MAX_DISTANCE);
    if (maxDistance <= 0.0f) return hdr;
    float endpointDistance = IsFarDepth(ndcDepth)
        ? maxDistance
        : max(dot(sceneWorldPos - rayOrigin, rayDirection), 0.0f);
    endpointDistance = min(endpointDistance, maxDistance);

    uint interfaceWidth = 0u;
    uint interfaceHeight = 0u;
    g_waterInterface.GetDimensions(interfaceWidth, interfaceHeight);
    if (interfaceWidth > 0u && interfaceHeight > 0u)
    {
        const int2 interfacePixel = clamp((int2)(uv * float2(interfaceWidth, interfaceHeight)),
                                           int2(0, 0), int2(interfaceWidth, interfaceHeight) - 1);
        const float4 interfaceValue = g_waterInterface.Load(int3(interfacePixel, 0));
        if (interfaceValue.x > 0.0f && interfaceValue.w > 0.5f)
        {
            const float3 cameraForward = normalize(float3(view[0][2], view[1][2], view[2][2]));
            const float viewRayCosine = dot(rayDirection, cameraForward);
            if (viewRayCosine > 1.0e-4f)
            {
                const float interfaceViewDepth = interfaceValue.y * farZ;
                const float interfaceDistance = max(interfaceViewDepth / viewRayCosine, 0.0f);
                endpointDistance = min(endpointDistance, interfaceDistance);
            }
        }
    }
    if (endpointDistance <= 1.0e-5f) return hdr;

    /// @note The candidate grid contains no water outside the global XZ bounds; retain the original march cells for intersecting rays.
    const float4 globalBounds = FBZZ_WaterMediumRow(1u);
    float boundsStart = 0.0f;
    float boundsEnd = endpointDistance;
    [branch]
    if (!FBZZ_WaterClipSlab(rayOrigin.x, rayDirection.x, globalBounds.x, globalBounds.z, boundsStart, boundsEnd)
        || !FBZZ_WaterClipSlab(rayOrigin.z, rayDirection.z, globalBounds.y, globalBounds.w, boundsStart, boundsEnd))
        return hdr;
    float2 vertical;
    if (FBZZ_WaterMediumVerticalBounds(false, vertical)) {
        if (!FBZZ_WaterClipSlab(rayOrigin.y, rayDirection.y, vertical.x, vertical.y, boundsStart, boundsEnd)) return hdr;
    }

    FBZZWaterMediumSample initial;
    const bool initialReady = FBZZ_QueryWaterMediumWithContext(rayOrigin, context, false, initial);
    const uint initialOffset = FBZZ_WATER_MEDIUM_HEADER_ROWS + initial.ownerIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
    const float4 initialGeometry = initialReady ? FBZZ_WaterMediumRow(initialOffset) : 0.0f;
    const float4 initialFlags = initialReady ? FBZZ_WaterMediumRow(initialOffset + 11u) : 0.0f;
    const float4 initialSpans = initialReady ? FBZZ_WaterMediumRow(initialOffset + 10u) : 0.0f;
    /// @note Cache only a certified constant-depth rectangle with no plumes or competing footprint anywhere along the view ray; wave heights and dry intervals remain per cell.
    const bool eligibleOwner = initialReady && initialGeometry.w >= 1.0f && initialGeometry.z < 0.5f
        && initialFlags.w > 0.5f && initialSpans.w == 0.0f
        && FBZZ_WaterRayOwnerIsolated(initial.ownerIndex, rayOrigin, rayDirection, endpointDistance, context);
#if defined(FBZZ_WATER_CACHED_FAST)
    if (!eligibleOwner) discard;
    const bool cachedOwner = true;
#else
    /// @note Shared mode 3 draws only the complement of the fast pass; both variants evaluate this same owner predicate before any integration.
    if (underwaterOpticalMode > 2.5f && eligibleOwner) discard;
    const bool cachedOwner = eligibleOwner;
#endif
    const float4 cachedLimits = cachedOwner ? FBZZ_WaterMediumRow(initialOffset + 6u) : 0.0f;
    const float4 cachedTile = cachedOwner ? FBZZ_WaterMediumRow(initialOffset + 8u) : 0.0f;
    const float4 cachedBounds = cachedOwner ? FBZZ_WaterMediumRow(initialOffset + 9u) : 0.0f;
    const WaterOpticalMedium cachedOptics = WaterPrepareOpticalMedium(initial.absorption, initial.scattering);
    const bool cachedNoExclusions = cachedOwner && (uint)round(initialSpans.y) == 0u;
    float cachedStart = 0.0f;
    float cachedEnd = endpointDistance;
    /// @note The owner's fixed XZ slabs have the same interval for every cell; clip once while retaining wave-dependent Y clipping.
    if (cachedOwner && (!FBZZ_WaterClipSlab(rayOrigin.x, rayDirection.x, cachedBounds.x, cachedBounds.z, cachedStart, cachedEnd)
        || !FBZZ_WaterClipSlab(rayOrigin.z, rayDirection.z, cachedBounds.y, cachedBounds.w, cachedStart, cachedEnd))) return hdr;
    const bool cachedSingleExclusion = cachedOwner && (uint)round(initialSpans.y) == 1u;
    float cachedDryStart = endpointDistance;
    float cachedDryEnd = endpointDistance;
    if (cachedSingleExclusion) {
        const uint row = (uint)round(FBZZ_WaterMediumRow(3u).y) + (uint)round(initialSpans.x) * 4u;
        if (row + 3u >= context.tableRows) cachedDryStart = 0.0f;
        else {
            /// @note This box/ray intersection is fixed across march cells; intersect its interval with each wave-clipped cell instead of transforming the ray again.
            if (!FBZZ_WaterClipExclusion(row, rayOrigin, rayDirection, 0.0f, endpointDistance,
                context, cachedDryStart, cachedDryEnd)) cachedDryStart = cachedDryEnd = endpointDistance;
        }
    }
    /// @note With no ambient source, a certified homogeneous view segment needs one Beer evaluation instead of repeated owner/atlas queries.
    /// @see https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance Homogeneous-medium transmittance; dry-volume intervals remain exact.
    [branch]
    if (cachedOwner && all(ambientColor == 0.0f)) {
        if (FBZZ_WaterCertifiedSegment(initial.ownerIndex, rayOrigin, rayDirection, endpointDistance, context)) {
            const float wetDistance = FBZZ_WaterUnexcludedDistance(initial.ownerIndex, rayOrigin,
                rayDirection, 0.0f, endpointDistance, context);
            const WaterOpticalTransport segment = WaterEvaluateOpticalTransport(
                initial.absorption, initial.scattering, wetDistance);
            const float3 directionalScatter = underwaterLightReady > 0.5f
                ? texUnderwaterVolume.SampleLevel(sampLinearClamp, uv, 0.0f).rgb : 0.0f;
            return hdr * segment.transmittance + directionalScatter;
        }
    }

    const uint steps = (uint)clamp(round(underwaterLightSteps), 8.0f, 32.0f);
    const float stepDistance = endpointDistance / (float)steps;
    float cachedSegmentDistance = stepDistance;
    WaterOpticalTransport cachedSegment = (WaterOpticalTransport)0;
    if (cachedOwner) cachedSegment = WaterEvaluatePreparedTransport(cachedOptics, stepDistance);
    /// @note Keep the original integration lattice while omitting cells wholly outside the producer's conservative water union.
    const uint firstStep = min(steps, (uint)max(floor(boundsStart / stepDistance) - 1.0f, 0.0f));
    const uint lastStep = min(steps, (uint)max(ceil(boundsEnd / stepDistance) + 1.0f, 0.0f));
    float3 viewTransmittance = 1.0f;
    float3 ambientScatter = 0.0f;
    [loop]
    for (uint step = firstStep; step < lastStep; ++step)
    {
        float cellStart = (float)step * stepDistance;
        float cellEnd = (float)(step + 1u) * stepDistance;
        const float3 samplePosition = rayOrigin + rayDirection * ((cellStart + cellEnd) * 0.5f);
        FBZZWaterMediumSample medium;
        [branch]
        if (cachedOwner) {
            float4 atlasValue;
            if (!SampleCachedWaterColumn(initial.ownerIndex, samplePosition,
                cachedBounds, cachedTile, cachedLimits, initialGeometry.w - 1.0f, context, atlasValue)) continue;
            medium = initial;
            medium.surfaceY = cachedLimits.x + atlasValue.x;
            medium.depth = min(max(atlasValue.y, 0.0f), max(cachedLimits.z, 0.0f));
            const float depth = medium.surfaceY - samplePosition.y;
            if (medium.depth <= 0.0f || depth <= 0.0f || depth > medium.depth) continue;
        } else if (!FBZZ_QueryWaterMediumWithContext(samplePosition, context, false, medium)) continue;

        const uint bodyOffset = FBZZ_WATER_MEDIUM_HEADER_ROWS
                              + medium.ownerIndex * FBZZ_WATER_MEDIUM_BODY_ROWS;
        const float4 waterBounds = cachedOwner ? cachedBounds : FBZZ_WaterMediumRow(bodyOffset + 9u);
        bool footprintReady;
        if (cachedOwner) {
            cellStart = max(cellStart, cachedStart);
            cellEnd = min(cellEnd, cachedEnd);
            footprintReady = cellStart <= cellEnd;
        } else footprintReady = FBZZ_WaterClipSlab(rayOrigin.x, rayDirection.x, waterBounds.x, waterBounds.z,
                                cellStart, cellEnd)
            && FBZZ_WaterClipSlab(rayOrigin.z, rayDirection.z, waterBounds.y, waterBounds.w, cellStart, cellEnd);
        if (!footprintReady || !FBZZ_WaterClipSlab(rayOrigin.y, rayDirection.y, medium.surfaceY - medium.depth,
                                   medium.surfaceY, cellStart, cellEnd)) continue;

        const float wetDistance = cachedNoExclusions ? max(cellEnd - cellStart, 0.0f)
            : cachedSingleExclusion ? max(min(cellEnd, cachedDryStart) - cellStart, 0.0f)
                + max(cellEnd - max(cellStart, cachedDryEnd), 0.0f)
            : FBZZ_WaterUnexcludedDistance(medium.ownerIndex, rayOrigin,
            rayDirection, cellStart, cellEnd, context);
        if (wetDistance <= 1.0e-5f) continue;
        const float3 wetSamplePosition = rayOrigin + rayDirection * ((cellStart + cellEnd) * 0.5f);
        const float3 plumeScattering = cachedOwner ? 0.0f : FBZZ_WaterPointExcluded(medium.ownerIndex,
            wetSamplePosition, context) ? 0.0f : medium.plumeScattering;

        const float3 sigmaS = medium.scattering + plumeScattering;
        WaterOpticalTransport segment;
        if (cachedOwner) {
            /// @note Reuse only bit-equal wet lengths in this homogeneous owner; boundary/dry cells and float subtraction differences trigger the original Beer evaluation.
            if (wetDistance != cachedSegmentDistance) {
                cachedSegment = WaterEvaluatePreparedTransport(cachedOptics, wetDistance);
                cachedSegmentDistance = wetDistance;
            }
            segment = cachedSegment;
        }
        else segment = WaterEvaluateOpticalTransport(medium.absorption, sigmaS, wetDistance);
        /// @note Zero ambient radiance or scattering cannot contribute; do not trace a source path that is multiplied by zero.
        [branch]
        if (any(ambientColor != 0.0f) && medium.lightStrength > 0.0f
            && any(segment.singleScatterWeight > 0.0f))
        {
            const float verticalSourceDistance = max(medium.surfaceY - wetSamplePosition.y, 0.0f);
            float3 sourceTransmittance;
            [branch]
            if (cachedOwner) {
                /// @note The isolated view footprint also excludes other owners at every vertical source column; subtract its dry boxes before Beer attenuation.
                const float sourceDistance = min(verticalSourceDistance, medium.maxDistance);
                float sourceStart = 0.0f;
                float sourceEnd = sourceDistance;
                /// @note A clipped view cell can move the source midpoint outside a rotated footprint; match the single-body source helper's local rectangle interval.
                if (context.bodyCount == 1u)
                {
                    if (!FBZZ_WaterRectangleClipWetInterval(medium.ownerIndex, wetSamplePosition,
                        float3(0.0f, 1.0f, 0.0f), context, medium, sourceStart, sourceEnd)) sourceStart = sourceEnd = 0.0f;
                }
                const float sourceWetDistance = cachedNoExclusions ? max(sourceEnd - sourceStart, 0.0f)
                    : FBZZ_WaterUnexcludedDistance(medium.ownerIndex,
                    wetSamplePosition, float3(0.0f, 1.0f, 0.0f), sourceStart,
                    sourceEnd, context);
                sourceTransmittance = exp(-min((medium.absorption + medium.scattering)
                    * sourceWetDistance, WATER_OPTICAL_MAX_DEPTH));
            } else sourceTransmittance = FBZZ_WaterSourceRGBTransmittance(
                wetSamplePosition, float3(0.0f, 1.0f, 0.0f), verticalSourceDistance, context, medium);
            ambientScatter += viewTransmittance * sourceTransmittance * ambientColor * 0.49f
                            * segment.singleScatterWeight * medium.lightStrength;
        }
        viewTransmittance *= segment.transmittance;
    }

    float3 directionalScatter = 0.0f;
    [branch]
    if (underwaterLightReady > 0.5f)
        directionalScatter = texUnderwaterVolume.SampleLevel(sampLinearClamp, uv, 0.0f).rgb;
    return hdr * viewTransmittance + ambientScatter + directionalScatter;
}

float3 SampleHdrWithBloom(float2 uv)
{
    float3 color = texHDR.Sample(sampLinearClamp, uv).rgb;
    [branch]
    if (bloomIntensity > 0.0f)
        color += texBloom.Sample(sampLinearClamp, uv).rgb * bloomIntensity;
    return color;
}

float3 ApplyDepthOfFieldHDR(float3 hdr, float2 uv, float ndcDepth)
{
    if (dofBlurRadius <= 0.0f)
        return hdr;

    /// @note 焦点距離から離れたピクセルほど、固定 8 点サンプルのぼかしを強く混ぜる。
    /// @note 専用 CoC バッファを増やさない軽量版として、Composite 内で完結させる。
    float depth = LinearDepthFromNdc(ndcDepth);
    float blur = saturate(abs(depth - dofFocusDistance) / max(dofFocusRange, 0.001f));
    float2 radius = texelSize * dofBlurRadius * blur;

    float3 sum = hdr;
    sum += SampleHdrWithBloom(saturate(uv + float2( radius.x,  0.0f)));
    sum += SampleHdrWithBloom(saturate(uv + float2(-radius.x,  0.0f)));
    sum += SampleHdrWithBloom(saturate(uv + float2( 0.0f,  radius.y)));
    sum += SampleHdrWithBloom(saturate(uv + float2( 0.0f, -radius.y)));
    sum += SampleHdrWithBloom(saturate(uv + float2( radius.x,  radius.y)));
    sum += SampleHdrWithBloom(saturate(uv + float2(-radius.x,  radius.y)));
    sum += SampleHdrWithBloom(saturate(uv + float2( radius.x, -radius.y)));
    sum += SampleHdrWithBloom(saturate(uv + float2(-radius.x, -radius.y)));

    return lerp(hdr, sum / 9.0f, blur);
}

float3 ApplySharpenHDR(float3 hdr, float2 uv)
{
    if (sharpenStrength <= 0.0f)
        return hdr;

    /// @note 十字 4 近傍の平均を引いたアンシャープマスク。
    /// @note Sobel のような輪郭抽出より安価で、Bloom 後の HDR に自然な解像感を足せる。
    float2 r = texelSize * max(sharpenRadius, 0.25f);
    float3 blur =
        SampleHdrWithBloom(saturate(uv + float2( r.x, 0.0f))) +
        SampleHdrWithBloom(saturate(uv + float2(-r.x, 0.0f))) +
        SampleHdrWithBloom(saturate(uv + float2(0.0f,  r.y))) +
        SampleHdrWithBloom(saturate(uv + float2(0.0f, -r.y)));
    blur *= 0.25f;

    return max(hdr + (hdr - blur) * sharpenStrength, 0.0f);
}

float3 ApplyClarity(float3 ldr, float2 uv)
{
    if (clarityStrength <= 0.0f)
        return ldr;

    /// @note 少し広い近傍平均との差分を LDR に戻すローカルコントラスト補正。
    /// @note シャープ化より大きい面の明暗差を強調し、ディテールが眠い画を自然に引き締める。
    /// @note 中心も近傍と同じ経路 (HDR + Bloom → トーンマップ) で作り、その差だけを足す。
    /// @note 完成した ldr と比べると、近傍に乗っていない霧・SSR・被写界深度まで «差» として強調される。
    const float  exposureValue = ResolveExposure();
    float2 r = texelSize * max(clarityRadius, 0.5f);
    const float3 center = FinalOutput(SampleHdrWithBloom(uv), exposureValue);
    float3 blur =
        FinalOutput(SampleHdrWithBloom(saturate(uv + float2( r.x, 0.0f))), exposureValue) +
        FinalOutput(SampleHdrWithBloom(saturate(uv + float2(-r.x, 0.0f))), exposureValue) +
        FinalOutput(SampleHdrWithBloom(saturate(uv + float2(0.0f,  r.y))), exposureValue) +
        FinalOutput(SampleHdrWithBloom(saturate(uv + float2(0.0f, -r.y))), exposureValue);
    blur *= 0.25f;

    return saturate(ldr + (center - blur) * clarityStrength);
}

/// @note 放射ブラー — 画面中心から外向きへ数タップ伸ばす。

/// @note 中心から «外» か: 起爆の «押し出され感» は、視界の端が後ろへ流れることで出る。
/// @note 逆向き (外から中心) にすると «吸い込まれる» になり、被弾の演出になってしまう。
/// @note タップ数を固定するか: 強さで分岐すると、弱いフレームと強いフレームで
/// @note コストが変わり、一番負荷が高い瞬間 (集束の起爆) だけ可変になる。
float3 ApplyRadialBlur(Texture2D tex, SamplerState samp, float2 uv, float3 base, float strength)
{
    if (strength <= 1.0e-5f) return base;
    const int   TAPS = 6;
    const float2 toCenter = (0.5f - uv) * strength;
    float3 sum = base;
    [unroll]
    for (int i = 1; i <= TAPS; ++i) {
        const float t = (float)i / (float)TAPS;
        sum += tex.SampleLevel(samp, uv + toCenter * t, 0).rgb;
    }
    return sum / (float)(TAPS + 1);
}

struct CompositeOutput
{
    float4 color : SV_Target0;
    float4 radiance : SV_Target1;
};
CompositeOutput PSMain(FBZZFullscreenVertex p)
{
    CompositeOutput output;
    output.color = float4(0.0f, 0.0f, 0.0f, 1.0f);
    output.radiance = 0.0f;
    float2 sourceUV = ApplyPixelateUV(p.uv, pixelSize);
    float2 uv = LensDistortUV(sourceUV, lensDistortion);
    if (any(uv < 0.0f) || any(uv > 1.0f))
        return output;
    /// @note 衝撃波リングは範囲判定の «後»。前に置くと、輪が画面端を抜ける最後の数フレームで
    /// @note ずらした先が範囲外になり、リングに沿って黒い帯が出る (端は sampLinearClamp が伸ばす)。
    uv = ShockRingUV(uv, shockRingCenter, shockRingRadius, shockRingWidth, shockRingAmplitude);
    bool coefficientUnderwater = underwaterOpticalMode > 0.5f && underwaterStrength > 0.0f;
    bool sharedWaterOptics = underwaterOpticalMode > 1.5f;
    if (coefficientUnderwater && !sharedWaterOptics && underwaterDistortion > 0.0f)
    {
        float wave = sin((uv.x + uv.y) * 28.0f + time * 2.4f);
        uv = saturate(uv + float2(wave, wave * 0.5f) * underwaterDistortion * underwaterStrength);
    }

    float3 hdr;
    [branch]
    if (abs(chromaticAberration) > 1.0e-5f)
    {
        const float2 caOffset = (uv - 0.5f) * chromaticAberration;
        hdr.r = texHDR.Sample(sampLinearClamp, uv + caOffset).r;
        hdr.g = texHDR.Sample(sampLinearClamp, uv).g;
        hdr.b = texHDR.Sample(sampLinearClamp, uv - caOffset).b;
    }
    else
    {
        hdr = texHDR.Sample(sampLinearClamp, uv).rgb;
    }
    /// @note 放射ブラーは色収差の «後»。先に掛けると RGB のずれごと引き伸ばされ、
    /// @note 中心から外へ虹が走る (収差ではなくプリズムに見える)。
    hdr = ApplyRadialBlur(texHDR, sampLinearClamp, uv, hdr, radialBlur);
    [branch]
    if (bloomIntensity > 0.0f)
    {
        hdr += texBloom.Sample(sampLinearClamp, uv).rgb * bloomIntensity;
    }
    /// @note 深度を読まないときは最遠 (Reversed-Z で 0) とみなす。
    /// @note フロクセル霧も深度を使う。条件から漏らすと全画素が空扱いになり、手前まで霧で覆われる。
    float ndcDepth = 0.0f;
    [branch]
    if (dofBlurRadius > 0.0f || fogDensity > 0.0f || underwaterStrength > 0.0f || froxelGridZ != 0u)
        ndcDepth = texDepth.Sample(sampLinearClamp, uv).r;
    hdr = ApplyDepthOfFieldHDR(hdr, uv, ndcDepth);
    hdr = ApplySharpenHDR(hdr, uv);

    /// @note 鏡面反射は不透明 HDR の段で解決済み。透明色や Bloom をここで置き換えない。
    /// @note Volumetric Light はここでは足さない。VolumetricLightPass が «水・半透明より前» で
    /// @note HDR へ加算済み。ここで足すとレイの終端 (不透明深度) までの光芒が水面の手前へ
    /// @note 描かれ、水が光の靄で塗り潰される。

    /// @note フロクセル霧は HDR のまま、トーンマップより前に乗せる。
    /// @note 霧は光そのもので、露出とトーンマップを一緒に受けるべきもの。
    /// @note LDR 側で足すと、明るいシーンで霧だけが白飛びして浮く。
    [branch]
    if (coefficientUnderwater)
    {
        [branch]
        if (sharedWaterOptics)
            hdr = ApplySharedWaterOpticsHDR(hdr, uv, ndcDepth);
        else
            hdr = ApplyUnderwaterOpticsHDR(hdr, uv, ndcDepth);
    }
    else
        hdr = ApplyFroxelFog(hdr, uv, ndcDepth);

    /// @note 露出 → ACES トーンマップ → sRGB ガンマ補正
    const float resolvedExposure = ResolveExposure();
    float3 ldr = FinalOutput(hdr, resolvedExposure);
    /// @note HDR 平均は非線形の画面変換より先。旧 LDR 霧・空間効果は従来の LDR 履歴を使う。
    /// @see https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/ Exposure before tone mapping and gamma afterwards.
    const bool hdrHistoryCompatible = (coefficientUnderwater || (fogDensity <= 0.0f && clarityStrength <= 0.0f))
        && !(underwaterStrength > 0.0f && underwaterOpticalMode <= 0.5f)
        && pixelSize <= 1.0f && abs(lensDistortion) <= 1e-5f && shockRingAmplitude <= 1e-5f
        && (!coefficientUnderwater || sharedWaterOptics || underwaterDistortion <= 0.0f);
    output.radiance = float4(clamp(hdr * resolvedExposure, 0.0f, 65504.0f), hdrHistoryCompatible ? 1.0f : 0.0f);
    if (!coefficientUnderwater)
        ldr = ApplyClarity(ldr, uv);

    /// @note 深度から線形距離を復元してフォグを適用する。
    /// @note 深度 0 (Reversed-Z の最遠) はスカイドームなので霧を掛けない。
    /// @note フォグの適用ロジック (ApplyFog) は共通で、色の出どころだけ fogSource で切り替える (§3-3)。
    if (fogDensity > 0.0f && !coefficientUnderwater)
    {
        if (!IsFarDepth(ndcDepth))
        {
            float linDepth = LinearDepthFromNdc(ndcDepth);
            float dist     = max(linDepth - fogFar, 0.0f);
            float factor   = FogFactor(dist, fogDensity);

            float3 col = fogColor; /// @note Exponential: 固定フォグ色
            if (fogSource > 0.5f)
            {
                /// @note Atmosphere (エアリアルパースペクティブ): 視線方向の大気 in-scatter をフォグ色に使う。
                /// @note 太陽から離れた遠景は青く、太陽方向は暖色に霞む。空ドームの見た目と整合する。
                float3 worldPos  = ReconstructWorldPos(uv, ndcDepth, invViewProjection);
                float3 rayDir    = normalize(worldPos - cameraPos);
                float3 sunDir    = normalize(-lightDir);
                /// @note skyDimmer を使う (Skydome / SunMoon と同じ軸)。lightIntensity を使うと
                /// @note DirectionalLight を強めただけでエアリアルパースが空ドームと食い違う。
                float  scaled    = sunIntensity * skyDimmer;
                float3 inscatter = ComputeAtmosphericScattering(
                    rayDir, sunDir, rayleighScattering, mieScattering, mieG, scaled) * lightColor;
                /// @note フォグは LDR 空間で適用するため、in-scatter (HDR) を露出→トーンマップして合わせる。
                col = FinalOutput(inscatter, ResolveExposure());
            }
            ldr = ApplyFog(ldr, factor, col);
        }
    }

    /// @note 水没カメラ用の全画面補正。
    /// @note 水面そのものは Water パスで描くが、カメラが水中に入った時の吸収・濁り・視界歪みは
    /// @note 画面全体にかかる効果なので Composite で一括処理する。
    if (underwaterStrength > 0.0f && underwaterOpticalMode <= 0.5f)
    {
        float depthFactor = underwaterStrength;
        if (!IsFarDepth(ndcDepth))
        {
            float linDepth = LinearDepthFromNdc(ndcDepth);
            depthFactor = saturate((1.0f - exp(-underwaterFogDensity * linDepth)) * underwaterStrength);
        }

        float wave = sin((uv.x + uv.y) * 28.0f + time * 2.4f) * 0.003f * underwaterStrength;
        float2 distortedUV = saturate(uv + float2(wave, wave * 0.5f));
        float3 distortedHdr = texHDR.Sample(sampLinearClamp, distortedUV).rgb;
        float3 distortedLdr = FinalOutput(distortedHdr, ResolveExposure());

        ldr = lerp(ldr, distortedLdr, underwaterStrength * 0.25f);
        ldr = lerp(ldr, underwaterColor, depthFactor);

        float gray = dot(ldr, float3(0.299f, 0.587f, 0.114f));
        ldr = lerp(float3(gray, gray, gray), ldr, lerp(1.0f, 0.55f, underwaterStrength));
    }

    output.color = float4(ApplyFinalColorEffects(ldr, uv, texLUT, sampLinearClamp), 1.0f);
    return output;
}
