/// @file    RayDielectricInterface.hlsli
/// @brief   Shared smooth dielectric interfaces and closed-medium attenuation.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#ifndef FBZZ_RAY_DIELECTRIC_INTERFACE_HLSLI
#define FBZZ_RAY_DIELECTRIC_INTERFACE_HLSLI

/// @note eta is transmitted / incident IOR. Both sides use the same exact Fresnel and TIR condition.
/// @see https://pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission Dielectric Fresnel
float RayDielectricFresnel(float cosine, float eta)
{
    cosine = saturate(cosine);
    if (eta == 1) return 0;
    float sineSquaredT = max(0, 1 - cosine * cosine) / (eta * eta);
    if (sineSquaredT >= 1) return 1;
    float cosineT = sqrt(max(0, 1 - sineSquaredT));
    float parallel = (eta * cosine - cosineT) / (eta * cosine + cosineT);
    float perpendicular = (cosine - eta * cosineT) / (cosine + eta * cosineT);
    return saturate(0.5f * (parallel * parallel + perpendicular * perpendicular));
}
bool RayDielectricSmooth(float roughness) { return roughness * roughness < 1e-3f; }
/// @note A smooth thin sheet sums both interfaces and their internal reflections; transmission stays straight and does not enter a medium.
/// @see https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF ThinDielectricBxDF effective R = 2R/(1+R)
float RayThinReflectance(float cosine, float eta)
{
    float reflection = RayDielectricFresnel(cosine, eta);
    return 2 * reflection / (1 + reflection);
}

/// @note Hybrid accepts constant rough/smooth solids and smooth white thin sheets; alpha coverage is not optical transmission.
bool RayHybridDielectricSupported(RaySurfaceRecord surface)
{
    bool thin = (surface.dielectricFlags & 1u) != 0;
    return surface.supported == 2u && surface.transmission == 1 && surface.metallic == 0
        && surface.baseColor.a == 1 && all(surface.emission == 0) && surface.textureMask == 0u
        && isfinite(surface.roughness) && surface.roughness >= 0 && surface.roughness <= 1
        && (!thin || (surface.roughness == 0 && all(surface.attenuationColor == 1)))
        && isfinite(surface.ior) && surface.ior > 0
        && all(isfinite(surface.attenuationColor)) && all(surface.attenuationColor >= 0) && all(surface.attenuationColor <= 1)
        && isfinite(surface.attenuationDistance) && surface.attenuationDistance > 0;
}

/// @note The owner includes scene identity; optical values must match on LIFO exit, including a multi-submesh solid.
struct RayClosedMedium
{
    uint objectIndex, objectGeneration, sceneGenerationLow, sceneGenerationHigh;
    float ior;
    float3 attenuationColor;
    float attenuationDistance;
};
bool RaySameMediumOwner(RayClosedMedium first, RayClosedMedium second)
{
    return first.objectIndex == second.objectIndex && first.objectGeneration == second.objectGeneration
        && first.sceneGenerationLow == second.sceneGenerationLow && first.sceneGenerationHigh == second.sceneGenerationHigh;
}
bool RaySameMedium(RayClosedMedium first, RayClosedMedium second)
{
    return RaySameMediumOwner(first, second) && first.ior == second.ior
        && all(first.attenuationColor == second.attenuationColor) && first.attenuationDistance == second.attenuationDistance;
}
/// @note Zero distance has unit transmittance; zero-color channels absorb every positive distance without evaluating log(0)*0.
/// @see https://pbr-book.org/4ed/Volume_Scattering/Transmittance Beer attenuation over actual boundary-to-boundary distance
float RayAbsorptionChannel(float color, float distanceRatio)
{
    if (distanceRatio == 0 || color == 1) return 1;
    if (color == 0) return 0;
    return exp(log(color) * distanceRatio);
}
float3 RayClosedMediumTransmittance(RayClosedMedium medium, float distance)
{
    float ratio = distance / medium.attenuationDistance;
    return float3(RayAbsorptionChannel(medium.attenuationColor.x, ratio),
        RayAbsorptionChannel(medium.attenuationColor.y, ratio), RayAbsorptionChannel(medium.attenuationColor.z, ratio));
}

/// @note Both Fresnel branches are evaluated, not selected with roulette. Solid transmission carries radiance-mode (etaI/etaT)^2.
/// @note Geometric-hemisphere null branches keep zero weight; the physical normal is never reoriented using the shading normal.
/// @return 0=null interface, 1=finite branches, 2=invalid arithmetic.
/// @see https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF Smooth dielectric and ThinDielectricBxDF
uint RaySplitSmoothDielectric(RaySurfaceRecord surface, float3 normal, float3 geometricNormal,
    bool frontFace, float3 incident, float etaI, float etaT, out float3 reflectedDirection,
    out float3 transmittedDirection, out float reflectionWeight, out float transmissionWeight)
{
    reflectedDirection = transmittedDirection = 0;
    reflectionWeight = transmissionWeight = 0;
    if (!RayHybridDielectricSupported(surface) || !isfinite(etaI) || !isfinite(etaT) || etaI <= 0 || etaT <= 0) return 2u;
    if (!frontFace) { normal = -normal; geometricNormal = -geometricNormal; }
    float cosineI = dot(normal, -incident);
    if (cosineI <= 0 || dot(normal, geometricNormal) <= 0) return 0u;
    float eta = etaT / etaI;
    if (!isfinite(eta) || eta <= 0) return 2u;
    bool thin = (surface.dielectricFlags & 1u) != 0;
    float reflectance = thin ? RayThinReflectance(cosineI, eta) : RayDielectricFresnel(cosineI, eta);
    if (!isfinite(reflectance)) return 2u;
    if (reflectance > 0) {
        reflectedDirection = normalize(reflect(incident, normal));
        if (!all(isfinite(reflectedDirection))) return 2u;
        if (dot(reflectedDirection, geometricNormal) > 0) reflectionWeight = reflectance;
    }
    if (reflectance < 1) {
        float ratio = etaI / etaT;
        transmittedDirection = thin || etaI == etaT ? incident : refract(incident, normal, ratio);
        float lengthSquared = dot(transmittedDirection, transmittedDirection);
        if (!isfinite(lengthSquared) || lengthSquared <= 0) return 2u;
        transmittedDirection *= rsqrt(lengthSquared);
        float radianceScale = thin ? 1 : ratio * ratio;
        if (!isfinite(radianceScale) || radianceScale <= 0) return 2u;
        if (dot(transmittedDirection, geometricNormal) < 0) transmissionWeight = (1 - reflectance) * radianceScale;
    }
    return isfinite(reflectionWeight) && isfinite(transmissionWeight) ? 1u : 2u;
}

#endif /// @note FBZZ_RAY_DIELECTRIC_INTERFACE_HLSLI
