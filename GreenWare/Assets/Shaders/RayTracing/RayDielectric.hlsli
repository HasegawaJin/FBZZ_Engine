/// @file    RayDielectric.hlsli
/// @brief   Radiance-mode smooth/thin and GGX solid dielectric evaluation and sampling.
/// @author  Hasegawa Jin
/// @date    2026-10-01
#ifndef FBZZ_RAY_DIELECTRIC_HLSLI
#define FBZZ_RAY_DIELECTRIC_HLSLI

#include "RayTracing/RayDielectricInterface.hlsli"
/// @note Full reflection/transmission PDF includes Fresnel branch probability and the refractive half-vector Jacobian.
/// @note Transmission is radiance mode: f is divided by (etaT/etaI)^2; sampling uses the same full f/pdf.
/// @see https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF Rough DielectricBxDF f, Sample_f and PDF
float3 EvaluateRayDielectric(float3 normal, float3 geometricNormal, float3 view, float3 direction,
    float roughness, float etaI, float etaT, out float pdf)
{
    pdf = 0;
    if (RayDielectricSmooth(roughness) || etaI == etaT) return 0;
    if (dot(geometricNormal, view) < 0) { normal = -normal; geometricNormal = -geometricNormal; }
    if (dot(normal, geometricNormal) <= 0) return 0;
    float nv = dot(normal, view), nl = dot(normal, direction);
    if (nv <= 0 || nl == 0) return 0;
    bool reflection = nl > 0;
    if ((dot(geometricNormal, direction) > 0) != reflection) return 0;
    float eta = etaT / etaI;
    float3 halfUnnormalized = view + direction * (reflection ? 1 : eta);
    if (dot(halfUnnormalized, halfUnnormalized) == 0) return 0;
    float3 halfVector = normalize(halfUnnormalized);
    if (dot(normal, halfVector) < 0) halfVector = -halfVector;
    float vh = dot(view, halfVector), lh = dot(direction, halfVector);
    if (vh * nv <= 0 || lh * nl <= 0) return 0;
    float alpha = roughness * roughness, nh = dot(normal, halfVector);
    float denominator = nh * nh * alpha * alpha + max(0, 1 - nh * nh);
    float distribution = alpha * alpha / (PI * denominator * denominator);
    float g1 = 1 / (1 + Lambda(nv, alpha));
    float geometry = 1 / (1 + Lambda(nv, alpha) + Lambda(nl, alpha));
    float fresnel = RayDielectricFresnel(vh, eta);
    float normalPdf = distribution * g1 * abs(vh) / nv;
    if (reflection) {
        pdf = normalPdf * fresnel / (4 * abs(vh));
        return (distribution * geometry * fresnel / abs(4 * nv * nl)).xxx;
    }
    float jacobianDenominator = lh + vh / eta;
    jacobianDenominator *= jacobianDenominator;
    if (jacobianDenominator <= 0) return 0;
    pdf = normalPdf * (1 - fresnel) * abs(lh) / jacobianDenominator;
    float value = distribution * geometry * (1 - fresnel) * abs(lh * vh / (jacobianDenominator * nv * nl));
    return (value / (eta * eta)).xxx;
}
/// @note A sampled Fresnel branch must remain in its macro hemisphere; evaluation must not reclassify an invalid reflection as transmission or change the medium stack.
/// @see https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF DielectricBxDF::Sample_f SameHemisphere checks
uint SampleRayRoughDielectric(float3 normal, float3 geometricNormal, float3 incident, float roughness,
    float etaI, float etaT, inout uint rng, out float3 direction, out float3 weight, out float pdf,
    out bool transmitted, out float etaRatioSquared)
{
    direction = weight = 0; pdf = 0; transmitted = false; etaRatioSquared = 1;
    if (dot(geometricNormal, -incident) < 0) { normal = -normal; geometricNormal = -geometricNormal; }
    if (dot(normal, geometricNormal) <= 0 || dot(normal, -incident) <= 0) return 0u;
    float3 tangent, bitangent;
    Basis(normal, tangent, bitangent);
    float3 view = -incident;
    float3 localView = float3(dot(view, tangent), dot(view, bitangent), dot(view, normal));
    if (localView.z <= 0) return 0u;
    float3 localHalf = VisibleNormal(localView, roughness * roughness, Random2(rng));
    float3 halfVector = localHalf.x * tangent + localHalf.y * bitangent + localHalf.z * normal;
    float eta = etaT / etaI;
    float fresnel = RayDielectricFresnel(dot(view, halfVector), eta);
    if (!isfinite(fresnel) || !isfinite(eta) || eta <= 0) return 2u;
    if (Random(rng) < fresnel) direction = reflect(incident, halfVector);
    else {
        direction = refract(incident, halfVector, 1 / eta);
        if (dot(direction, direction) <= 0) return 0u;
        transmitted = true;
        etaRatioSquared = (1 / eta) * (1 / eta);
    }
    direction = normalize(direction);
    float cosine = dot(normal, direction);
    float geometricCosine = dot(geometricNormal, direction);
    if (transmitted ? (cosine >= 0 || geometricCosine >= 0) : (cosine <= 0 || geometricCosine <= 0)) return 0u;
    float3 value = EvaluateRayDielectric(normal, geometricNormal, view, direction, roughness, etaI, etaT, pdf);
    weight = pdf > 0 ? value * abs(dot(normal, direction)) / pdf : 0;
    if (!all(isfinite(direction)) || !all(isfinite(weight)) || !isfinite(pdf)
        || !isfinite(etaRatioSquared) || etaRatioSquared <= 0) return 2u;
    return pdf > 0 ? 1u : 0u;
}
#endif
