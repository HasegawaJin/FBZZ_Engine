/// @file    RayHybridGlassTraversalLegacy.hlsli
/// @brief   Frozen pre-tail-continuation traversal oracle for production glass equivalence.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#ifndef FBZZ_RAY_HYBRID_GLASS_TRAVERSAL_LEGACY_HLSLI
#define FBZZ_RAY_HYBRID_GLASS_TRAVERSAL_LEGACY_HLSLI

/// @note Only traversal is frozen; both paths use the current production Fresnel, GGX, medium and motion helpers.
bool LegacyHybridGlassRadianceWithMotion(HybridGlassPath initial, SurfaceHit firstHit, inout uint rng,
    out float3 radiance, out RayReflectionGlassMotion motion)
{
    radiance = 0; motion = (RayReflectionGlassMotion)0;
    if (!glassEnabled || !glassBoundaryLimit
        || (firstHit.surface.supported != 1u && !RayHybridDielectricSupported(firstHit.surface))) return false;
    HybridGlassPath pending[17];
    pending[0] = initial;
    uint pendingCount = 1u;
    bool first = true;
    uint boundaryLimit = min(glassBoundaryLimit, 16u);
    [loop] for (uint work = 0; work < 512u && pendingCount; ++work) {
        HybridGlassPath path = pending[--pendingCount];
        RayClosedMedium active = (RayClosedMedium)0;
        if (path.mediumDepth && !HybridLoadMedium(path.media[path.mediumDepth - 1u], active)) return false;
        SurfaceHit hit;
        uint status = 1u;
        if (first) { hit = firstHit; first = false; }
        else {
            status = TraceSurface(path.ray, path.mask, hit);
            if (path.mediumDepth && path.mask != 1u) {
                SurfaceHit ownerBoundary;
                uint ownerStatus = TraceMeshSurface(path.ray, 1u, ownerBoundary, 0u, active.objectIndex, active.objectGeneration);
                if (ownerStatus == 2u) return false;
                if (ownerStatus == 1u && (status == 0u || ownerBoundary.distance <= hit.distance)) {
                    status = 1u; hit = ownerBoundary;
                }
            }
        }
        if (status == 2u) return false;
        if (status == 0u) {
            if (path.mediumDepth || environmentMode == 0u) return false;
            float3 value = path.throughput * RayEnvironmentRadiance(path.ray.Direction, environmentMode,
                constantEnvironmentRadiance, envRotation, envIntensity);
            radiance += value;
            if (path.motionBranch && environmentMode == 1u) {
                RayReflectionMotionGuide guide = (RayReflectionMotionGuide)0;
                guide.terminalPositionParameter = float4(path.ray.Direction, 0);
                guide.objectPrimitiveKind.w = 2u;
                if (path.motionBranch == 1u) { motion.reflected = guide; motion.reflectedRadiance = value; }
                else { motion.transmitted = guide; motion.transmittedRadiance = value; }
            }
            continue;
        }
        if (path.mediumDepth) {
            float3 segmentDirection; float segmentDistance;
            if (!LightSegment(hit.position - path.previousPosition, segmentDirection, segmentDistance)) return false;
            path.throughput *= RayClosedMediumTransmittance(active, segmentDistance);
        }
        if (!all(isfinite(path.throughput)) || any(path.throughput < 0)) return false;
        if (all(path.throughput == 0)) continue;
        if (hit.surface.supported != 2u) {
            if (path.mediumDepth) {
                hit.inMedium = 1u; hit.mediumAttenuationColor = active.attenuationColor;
                hit.mediumAttenuationDistance = active.attenuationDistance;
            }
            float3 value = path.throughput * HitRadiance(hit, -path.ray.Direction, rng);
            radiance += value;
            HybridStoreMotionTerminal(path, hit, value, motion);
            continue;
        }
        if (!RayHybridDielectricSupported(hit.surface)) return false;
        bool thin = (hit.surface.dielectricFlags & 1u) != 0;
        RayClosedMedium boundary = HybridHitMedium(hit);
        float etaI = path.mediumDepth ? active.ior : 1;
        float etaT = hit.surface.ior;
        if (!thin) {
            if (hit.frontFace) {
                if (path.mediumDepth == 8u) return false;
                [unroll] for (uint i = 0; i < 8u; ++i) {
                    if (i >= path.mediumDepth) continue;
                    RayClosedMedium ancestor;
                    if (!HybridLoadMedium(path.media[i], ancestor) || RaySameMediumOwner(ancestor, boundary)) return false;
                }
            } else {
                if (!path.mediumDepth) {
                    if (HybridUnresolvedBoundary(path.ray, path.mask, hit)) continue;
                    return false;
                }
                if (!RaySameMedium(active, boundary)) return false;
                etaT = 1;
                if (path.mediumDepth > 1u) {
                    RayClosedMedium parent;
                    if (!HybridLoadMedium(path.media[path.mediumDepth - 2u], parent)) return false;
                    etaT = parent.ior;
                }
            }
        }
        if (path.boundaryCount >= boundaryLimit) continue;
        bool firstThin = thin && path.boundaryCount == 0u && path.mediumDepth == 0u;
        path.motionBranch = 0u;
        if (!thin && !RayDielectricSmooth(hit.surface.roughness) && etaI != etaT) {
            float3 direction, weight; float pdf, etaRatioSquared; bool transmitted;
            uint sampled = SampleRayRoughDielectric(hit.normal, hit.geometricNormal, path.ray.Direction,
                hit.surface.roughness, etaI, etaT, rng, direction, weight, pdf, transmitted, etaRatioSquared);
            if (sampled == 2u) return false;
            if (sampled == 0u) continue;
            if (pendingCount >= 17u) return false;
            path.throughput *= weight; path.previousPosition = hit.position; ++path.boundaryCount;
            path.ray.Origin = OffsetOrigin(hit, direction); path.ray.Direction = direction;
            path.ray.TMin = 0; path.ray.TMax = 3.402823466e38f;
            if (transmitted) {
                if (hit.frontFace) path.media[path.mediumDepth++] = hit.instanceId;
                else --path.mediumDepth;
            } else path.mask = 4u;
            pending[pendingCount++] = path;
            continue;
        }
        float3 reflectedDirection, transmittedDirection;
        float reflectionWeight, transmissionWeight;
        uint split = RaySplitSmoothDielectric(hit.surface, hit.normal, hit.geometricNormal, hit.frontFace,
            path.ray.Direction, etaI, etaT, reflectedDirection, transmittedDirection, reflectionWeight, transmissionWeight);
        if (split == 2u) return false;
        if (split == 0u) continue;
        path.previousPosition = hit.position;
        ++path.boundaryCount;
        path.ray.TMin = 0; path.ray.TMax = 3.402823466e38f;
        /// @note An index-matched thin interface has exactly zero reflected energy; its constant branch needs no scene query or terminal correspondence.
        if (firstThin && environmentMode == 1u && reflectionWeight == 0) {
            motion.reflected = (RayReflectionMotionGuide)0;
            motion.reflected.objectPrimitiveKind.w = 2u;
            motion.reflectedRadiance = 0;
        }
        if (reflectionWeight > 0) {
            if (pendingCount >= 17u) return false;
            HybridGlassPath reflected = path;
            reflected.motionBranch = firstThin ? 1u : 0u;
            reflected.mask = 4u;
            reflected.throughput *= reflectionWeight;
            reflected.ray.Origin = OffsetOrigin(hit, reflectedDirection); reflected.ray.Direction = reflectedDirection;
            pending[pendingCount++] = reflected;
        }
        if (transmissionWeight > 0) {
            if (pendingCount >= 17u) return false;
            path.throughput *= transmissionWeight;
            path.motionBranch = firstThin ? 2u : 0u;
            path.ray.Origin = OffsetOrigin(hit, transmittedDirection); path.ray.Direction = transmittedDirection;
            if (!thin && hit.frontFace) path.media[path.mediumDepth++] = hit.instanceId;
            else if (!thin) --path.mediumDepth;
            pending[pendingCount++] = path;
        }
    }
    return !pendingCount && all(isfinite(radiance)) && all(radiance >= 0);
}

#endif
