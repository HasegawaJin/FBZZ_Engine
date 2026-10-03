/// @file    RayHybridGlass.hlsli
/// @brief   Bounded deterministic or single-path Hybrid glass transport over verified ray surfaces.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#ifndef FBZZ_RAY_HYBRID_GLASS_HLSLI
#define FBZZ_RAY_HYBRID_GLASS_HLSLI

#include "RayTracing/RayReflectionMotion.hlsli"

void HybridGlassBasis(float3 normal, out float3 tangent, out float3 bitangent)
{
    tangent = normalize(cross(abs(normal.z) < 0.999f ? float3(0, 0, 1) : float3(0, 1, 0), normal));
    bitangent = cross(normal, tangent);
}
float2 HybridGlassRandom2(inout uint rng)
{
    float first = Random(rng), second = Random(rng);
    return float2(first, second);
}
/// @note Reuse the Reference full GGX dielectric f/pdf with Hybrid's identical VNDF and RNG primitives.
#define Basis HybridGlassBasis
#define VisibleNormal SampleVisibleNormal
#define Lambda SmithLambda
#define Random2 HybridGlassRandom2
#include "RayTracing/RayDielectric.hlsli"
#undef Random2
#undef Lambda
#undef VisibleNormal
#undef Basis

RayClosedMedium HybridHitMedium(SurfaceHit hit)
{
    RayClosedMedium medium;
    medium.objectIndex = hit.objectIndex; medium.objectGeneration = hit.objectGeneration;
    medium.sceneGenerationLow = hit.sceneGenerationLow; medium.sceneGenerationHigh = hit.sceneGenerationHigh;
    medium.ior = hit.surface.ior; medium.attenuationColor = hit.surface.attenuationColor;
    medium.attenuationDistance = hit.surface.attenuationDistance;
    return medium;
}

/// @note Pending paths retain only dense table IDs; the immutable submitted tables preserve owner and optics across a multi-submesh exit.
bool HybridLoadMedium(uint instanceId, out RayClosedMedium medium)
{
    medium = (RayClosedMedium)0;
    if (instanceId >= instanceCount) return false;
    StructuredBuffer<RayHitRecord> records = ResourceDescriptorHeap[FbzzPixelSlot(1)];
    StructuredBuffer<RaySurfaceRecord> surfaces = ResourceDescriptorHeap[FbzzPixelSlot(2)];
    RayHitRecord record = records[instanceId];
    RaySurfaceRecord surface = surfaces[instanceId];
    if (!RayHybridDielectricSupported(surface) || (surface.dielectricFlags & 1u) != 0) return false;
    medium.objectIndex = record.objectIndex; medium.objectGeneration = record.objectGeneration;
    medium.sceneGenerationLow = record.sceneGenerationLow; medium.sceneGenerationHigh = record.sceneGenerationHigh;
    medium.ior = surface.ior; medium.attenuationColor = surface.attenuationColor;
    medium.attenuationDistance = surface.attenuationDistance;
    return true;
}

/// @note Only a same-owner, same-optics entry within one FP32 T ULP certifies an unresolved boundary pair; no cosine-expanded tolerance is used.
/// @see https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#ray-flags Closest front-only query
bool HybridUnresolvedBoundary(RayDesc ray, uint mask, SurfaceHit exitHit)
{
    if (!isfinite(exitHit.distance) || exitHit.distance <= 0) return false;
    uint exitBits = asuint(exitHit.distance);
    ray.TMax = min(ray.TMax, asfloat(min(exitBits + 2u, 0x7F7FFFFFu)));
    SurfaceHit entry;
    if (TraceSurface(ray, mask, entry, RAY_FLAG_CULL_BACK_FACING_TRIANGLES) != 1u || !entry.frontFace
        || !RayHybridDielectricSupported(entry.surface) || !RaySameMedium(HybridHitMedium(entry), HybridHitMedium(exitHit))
        || !isfinite(entry.distance) || entry.distance <= 0) return false;
    uint entryBits = asuint(entry.distance);
    return (entryBits > exitBits ? entryBits - exitBits : exitBits - entryBits) <= 1u;
}

struct HybridGlassPath
{
    RayDesc ray;
    float3 throughput, previousPosition;
    uint mediumDepth, boundaryCount, mask;
    uint media[8];
    uint motionBranch;
};

/// @note A closed owner contains the origin only when both opposite owner-only closest queries exit it; exit order must agree in both directions.
/// @note Owner/optics are exact and initial nesting is capped at eight. Open or coincident/ambiguous intervals fail closed, not air.
/// @see https://pbr-book.org/4ed/Volume_Scattering/Transmittance Media boundaries and actual segment transmittance
bool HybridInitializeGlassPath(RayDesc ray, uint mask, out HybridGlassPath path)
{
    path = (HybridGlassPath)0;
    path.ray = ray; path.throughput = 1; path.previousPosition = ray.Origin; path.mask = mask;
    if (!all(isfinite(ray.Origin)) || !all(isfinite(ray.Direction)) || dot(ray.Direction, ray.Direction) <= 0) return false;
    if (!glassEnabled) return true;
    StructuredBuffer<RayHitRecord> records = ResourceDescriptorHeap[FbzzPixelSlot(1)];
    StructuredBuffer<RaySurfaceRecord> surfaces = ResourceDescriptorHeap[FbzzPixelSlot(2)];
    float exits[8], reverseExits[8];
    RayDesc probe = ray;
    probe.TMin = 0; probe.TMax = 3.402823466e38f;
    [loop] for (uint instance = 0; instance < instanceCount; ++instance) {
        if (surfaces[instance].supported != 2u || (surfaces[instance].dielectricFlags & 1u) != 0) continue;
        RayClosedMedium medium;
        if (!HybridLoadMedium(instance, medium)) return false;
        bool seen = false;
        [loop] for (uint previous = 0; previous < instance; ++previous) {
            RayHitRecord oldRecord = records[previous];
            if (oldRecord.objectIndex != medium.objectIndex || oldRecord.objectGeneration != medium.objectGeneration
                || oldRecord.sceneGenerationLow != medium.sceneGenerationLow || oldRecord.sceneGenerationHigh != medium.sceneGenerationHigh) continue;
            RayClosedMedium oldMedium;
            if (!HybridLoadMedium(previous, oldMedium) || !RaySameMedium(oldMedium, medium)) return false;
            seen = true;
        }
        if (seen) continue;
        SurfaceHit exitHit;
        uint status = TraceMeshSurface(probe, 1u, exitHit, 0u, medium.objectIndex, medium.objectGeneration);
        if (status == 2u) return false;
        if (status == 0u || exitHit.frontFace) continue;
        if (HybridUnresolvedBoundary(probe, 1u, exitHit)) continue;
        if (!RayHybridDielectricSupported(exitHit.surface) || !RaySameMedium(medium, HybridHitMedium(exitHit))
            || !isfinite(exitHit.distance) || exitHit.distance <= exitHit.offsetDistance) return false;
        RayDesc reverseProbe = probe; reverseProbe.Direction = -probe.Direction;
        SurfaceHit reverseExit;
        if (TraceMeshSurface(reverseProbe, 1u, reverseExit, 0u, medium.objectIndex, medium.objectGeneration) != 1u
            || reverseExit.frontFace || !RayHybridDielectricSupported(reverseExit.surface)
            || !RaySameMedium(medium, HybridHitMedium(reverseExit)) || !isfinite(reverseExit.distance)
            || reverseExit.distance <= reverseExit.offsetDistance || HybridUnresolvedBoundary(reverseProbe, 1u, reverseExit)) return false;
        if (path.mediumDepth >= 8u) return false;
        uint insertion = path.mediumDepth;
        for (uint i = 0; i < path.mediumDepth; ++i) {
            if (exitHit.distance == exits[i] || reverseExit.distance == reverseExits[i]
                || ((exitHit.distance > exits[i]) != (reverseExit.distance > reverseExits[i]))) return false;
            if (exitHit.distance > exits[i]) insertion = min(insertion, i);
        }
        for (uint j = path.mediumDepth; j > insertion; --j) {
            path.media[j] = path.media[j - 1]; exits[j] = exits[j - 1]; reverseExits[j] = reverseExits[j - 1];
        }
        path.media[insertion] = instance; exits[insertion] = exitHit.distance; reverseExits[insertion] = reverseExit.distance;
        ++path.mediumDepth;
    }
    return true;
}

void HybridStoreMotionTerminal(HybridGlassPath path, SurfaceHit hit, float3 value,
    inout RayReflectionGlassMotion motion)
{
    if (!path.motionBranch || path.mediumDepth || hit.virtualEmitter || hit.virtualShape) return;
    RayReflectionMotionGuide guide = (RayReflectionMotionGuide)0;
    guide.terminalPositionParameter = float4(hit.position, hit.offsetDistance);
    guide.objectPrimitiveKind = uint4(hit.objectIndex, hit.objectGeneration, hit.primitiveId, 1u);
    if (path.motionBranch == 1u) { motion.reflected = guide; motion.reflectedRadiance = value; }
    else { motion.transmitted = guide; motion.transmittedRadiance = value; }
}

/// @note Physical Fresnel probabilities retain geometric null samples and radiance-mode eta squared through existing weight/probability compensation.
/// @see https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF Sample perfect specular dielectric BSDF and ThinDielectricBxDF.
bool HybridSelectSmoothDielectric(SurfaceHit hit, float3 incident, float etaI, float etaT, inout uint rng,
    inout float reflectionWeight, inout float transmissionWeight)
{
    float3 normal = hit.frontFace ? hit.normal : -hit.normal;
    float cosine = dot(normal, -incident);
    float eta = etaT / etaI;
    float probability = (hit.surface.dielectricFlags & 1u) != 0
        ? RayThinReflectance(cosine, eta) : RayDielectricFresnel(cosine, eta);
    if (!isfinite(probability) || probability < 0 || probability > 1) return false;
    if (probability == 0) { reflectionWeight = 0; return true; }
    if (probability == 1) { transmissionWeight = 0; return true; }
    float choice = Random(rng);
    if (!isfinite(choice) || choice < 0 || choice >= 1) return false;
    if (choice < probability) { reflectionWeight /= probability; transmissionWeight = 0; }
    else { transmissionWeight /= 1 - probability; reflectionWeight = 0; }
    return isfinite(reflectionWeight) && isfinite(transmissionWeight);
}

/// @note Deterministic smooth Fresnel evaluates both branches; opt-in single-path visits one compensated branch, while rough sampling remains the shared full f*cos/pdf.
/// @note Single-path validates visited owners and arithmetic; invalid unselected branches cannot be discovered without evaluating the deterministic tree.
/// @note Each path has at most 16 boundaries; its remaining radiance is zero at that finite budget. Work over 512 visited states fails closed instead of publishing a partial result.
/// @note Verified initial closed media and opaque terminals retain their actual Beer segments; unknown environments, non-LIFO overlaps and open solids require fallback.
/// @note Transmission retains camera/reflection provenance; every reflected branch uses castReflection mask4 while its active solid's exit remains visible.
/// @return false leaves the producer's finite alpha0 output intact; no previous-frame glass value may substitute for failure.
/// @see https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF Radiance-mode Fresnel and refraction
/// @see https://pbr-book.org/4ed/Volume_Scattering/Transmittance Actual boundary-to-boundary medium distance
bool HybridGlassRadianceWithMotion(HybridGlassPath initial, SurfaceHit firstHit, inout uint rng,
    out float3 radiance, out RayReflectionGlassMotion motion)
{
    radiance = 0; motion = (RayReflectionGlassMotion)0;
    if (!glassEnabled || !glassBoundaryLimit
        || (firstHit.surface.supported != 1u && !RayHybridDielectricSupported(firstHit.surface))) return false;
    /// @note The legacy 17 slots held the current path plus at most 16 siblings; only siblings enter this stack.
    HybridGlassPath pending[16];
    HybridGlassPath path = initial;
    uint pendingCount = 0u;
    bool current = true;
    bool first = true;
    uint boundaryLimit = min(glassBoundaryLimit, 16u);
    [loop] for (uint work = 0; work < 512u && (current || pendingCount); ++work) {
        if (!current) path = pending[--pendingCount];
        current = false;
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
            current = true;
            continue;
        }
        float3 reflectedDirection, transmittedDirection;
        float reflectionWeight, transmissionWeight;
        uint split = RaySplitSmoothDielectric(hit.surface, hit.normal, hit.geometricNormal, hit.frontFace,
            path.ray.Direction, etaI, etaT, reflectedDirection, transmittedDirection, reflectionWeight, transmissionWeight);
        if (split == 2u) return false;
        if (split == 0u) continue;
        bool zeroReflectedEnergy = reflectionWeight == 0;
        if (HybridGlassSinglePathEnabled()
            && !HybridSelectSmoothDielectric(hit, path.ray.Direction, etaI, etaT, rng, reflectionWeight, transmissionWeight)) return false;
        path.previousPosition = hit.position;
        ++path.boundaryCount;
        path.ray.TMin = 0; path.ray.TMax = 3.402823466e38f;
        /// @note An index-matched thin interface has exactly zero reflected energy; its constant branch needs no scene query or terminal correspondence.
        if (firstThin && environmentMode == 1u && zeroReflectedEnergy) {
            motion.reflected = (RayReflectionMotionGuide)0;
            motion.reflected.objectPrimitiveKind.w = 2u;
            motion.reflectedRadiance = 0;
        }
        if (reflectionWeight > 0) {
            if (pendingCount >= 17u) return false;
            /// @note Two legacy pushes require two free slots; rejecting before storage preserves the same capacity failure without a seventeenth sibling.
            if (transmissionWeight > 0 && pendingCount >= 16u) return false;
            HybridGlassPath reflected = path;
            reflected.motionBranch = firstThin ? 1u : 0u;
            reflected.mask = 4u;
            reflected.throughput *= reflectionWeight;
            reflected.ray.Origin = OffsetOrigin(hit, reflectedDirection); reflected.ray.Direction = reflectedDirection;
            if (transmissionWeight > 0) pending[pendingCount++] = reflected;
            else { path = reflected; current = true; }
        }
        if (transmissionWeight > 0) {
            if (pendingCount >= 17u) return false;
            path.throughput *= transmissionWeight;
            path.motionBranch = firstThin ? 2u : 0u;
            path.ray.Origin = OffsetOrigin(hit, transmittedDirection); path.ray.Direction = transmittedDirection;
            if (!thin && hit.frontFace) path.media[path.mediumDepth++] = hit.instanceId;
            else if (!thin) --path.mediumDepth;
            current = true;
        }
    }
    return !current && !pendingCount && all(isfinite(radiance)) && all(radiance >= 0);
}

bool HybridGlassRadiance(HybridGlassPath initial, SurfaceHit firstHit, inout uint rng, out float3 radiance)
{
    RayReflectionGlassMotion ignored;
    return HybridGlassRadianceWithMotion(initial, firstHit, rng, radiance, ignored);
}

bool HybridGlassRadiance(RayDesc initialRay, SurfaceHit firstHit, uint mask, inout uint rng, out float3 radiance)
{
    HybridGlassPath initial;
    radiance = 0;
    if (!HybridInitializeGlassPath(initialRay, mask, initial)) return false;
    return HybridGlassRadiance(initial, firstHit, rng, radiance);
}

#endif /// @note FBZZ_RAY_HYBRID_GLASS_HLSLI
