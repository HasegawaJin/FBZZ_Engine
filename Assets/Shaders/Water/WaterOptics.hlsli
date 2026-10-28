/// @file    WaterOptics.hlsli
/// @brief   Homogeneous water transmittance and single-scattering helpers.
/// @author  Hasegawa Jin
/// @date    2026-10-07
#ifndef FBZZ_WATER_OPTICS_HLSLI
#define FBZZ_WATER_OPTICS_HLSLI

static const float WATER_OPTICAL_MAX_COEFFICIENT = 1000.0f;
static const float WATER_OPTICAL_MAX_DISTANCE = 1000000.0f;
static const float WATER_OPTICAL_MAX_DEPTH = 80.0f;
static const float WATER_OPTICAL_PHASE_G_LIMIT = 0.95f;
static const float WATER_OPTICAL_INV_FOUR_PI = 0.07957747154594767f;

struct WaterOpticalTransport
{
    float3 transmittance;
    float3 singleScatterWeight;
};

/// @note Prepared coefficients can be reused only while the medium and plume scattering remain unchanged.
struct WaterOpticalMedium
{
    float3 extinction;
    float3 albedo;
};

/// @brief Clamps a material extinction coefficient to a finite, nonnegative range.
/// @param coefficient Extinction coefficient in inverse meters.
/// @return Coefficient in [0, 1000] m^-1; non-finite negative-side values become 0.
float WaterOpticalSanitizeCoefficient(float coefficient)
{
    if (!(coefficient >= 0.0f))
        return 0.0f;
    return coefficient > WATER_OPTICAL_MAX_COEFFICIENT ? WATER_OPTICAL_MAX_COEFFICIENT : coefficient;
}

/// @brief Clamps the Henyey-Greenstein mean cosine while replacing NaN with isotropic scattering.
/// @param phaseG Mean cosine of the scattering distribution.
/// @return Value in [-0.95, 0.95].
float WaterOpticalClampPhaseG(float phaseG)
{
    if (phaseG != phaseG)
        return 0.0f;
    return clamp(phaseG, -WATER_OPTICAL_PHASE_G_LIMIT, WATER_OPTICAL_PHASE_G_LIMIT);
}

/// @brief Evaluates the normalized Henyey-Greenstein phase function.
/// @param cosTheta Dot product of incoming propagation (-L) and outgoing direction toward the camera (V).
/// @param phaseG Mean cosine; positive values favor forward scattering toward the camera along incoming light.
/// @return Scattering density in sr^-1; the isotropic case is 1/(4π).
/// @see https://www.pbr-book.org/4ed/Volume_Scattering/Phase_Functions PBRT v4, §11.3.1 Henyey–Greenstein phase function and direction convention.
/// @see https://dev.epicgames.com/documentation/unreal-engine/single-layer-water-shading-model-in-unreal-engine Epic Single Layer Water, absorption/scattering coefficients and PhaseG material controls.
float WaterHenyeyGreensteinPhase(float cosTheta, float phaseG)
{
    if (cosTheta != cosTheta)
        cosTheta = 0.0f;
    cosTheta = clamp(cosTheta, -1.0f, 1.0f);
    phaseG = WaterOpticalClampPhaseG(phaseG);

    float denominator = max(1.0f + phaseG * phaseG - 2.0f * phaseG * cosTheta, 1.0e-4f);
    return WATER_OPTICAL_INV_FOUR_PI * (1.0f - phaseG * phaseG)
         / (denominator * sqrt(denominator));
}

/// @brief Prepare reusable extinction and single-scattering albedo for one homogeneous medium.
/// @param absorption RGB absorption coefficients in m^-1.
/// @param scattering RGB scattering coefficients in m^-1.
/// @return Sanitized extinction and scattering/extinction; a zero-extinction channel has zero albedo.
/// @see https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance PBRT v4, §11.2 homogeneous Beer transmittance.
WaterOpticalMedium WaterPrepareOpticalMedium(float3 absorption, float3 scattering)
{
    absorption = float3(WaterOpticalSanitizeCoefficient(absorption.x),
                        WaterOpticalSanitizeCoefficient(absorption.y),
                        WaterOpticalSanitizeCoefficient(absorption.z));
    scattering = float3(WaterOpticalSanitizeCoefficient(scattering.x),
                        WaterOpticalSanitizeCoefficient(scattering.y),
                        WaterOpticalSanitizeCoefficient(scattering.z));

    float3 sigmaT = absorption + scattering;
    WaterOpticalMedium medium;
    medium.extinction = sigmaT;
    medium.albedo = float3(
        sigmaT.x > 0.0f ? scattering.x / sigmaT.x : 0.0f,
        sigmaT.y > 0.0f ? scattering.y / sigmaT.y : 0.0f,
        sigmaT.z > 0.0f ? scattering.z / sigmaT.z : 0.0f);
    return medium;
}

/// @brief Evaluate the same Beer and stable short-segment law from already sanitized coefficients.
/// @see https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance Homogeneous-medium transport.
WaterOpticalTransport WaterEvaluatePreparedTransport(WaterOpticalMedium medium, float pathLength)
{
    if (!(pathLength >= 0.0f)) pathLength = 0.0f;
    pathLength = min(pathLength, WATER_OPTICAL_MAX_DISTANCE);
    float3 opticalDepth = min(medium.extinction * pathLength, WATER_OPTICAL_MAX_DEPTH);
    WaterOpticalTransport result;
    result.transmittance = exp(-opticalDepth);
    /// @note 極小optical depthの1-exp差の桁落ちを避け、消散0の極限を連続に保つ。
    float3 shortSegment = opticalDepth * (1.0f - opticalDepth * (0.5f - opticalDepth / 6.0f));
    float3 removed = float3(
        opticalDepth.x < 0.001f ? shortSegment.x : 1.0f - result.transmittance.x,
        opticalDepth.y < 0.001f ? shortSegment.y : 1.0f - result.transmittance.y,
        opticalDepth.z < 0.001f ? shortSegment.z : 1.0f - result.transmittance.z);
    result.singleScatterWeight = medium.albedo * removed;
    return result;
}

/// @brief Evaluate Beer transmittance and stable integrated single-scattering weight from material coefficients.
/// @param pathLength Segment length in meters.
/// @see https://www.pbr-book.org/4ed/Volume_Scattering/Transmittance Homogeneous Beer transport.
WaterOpticalTransport WaterEvaluateOpticalTransport(float3 absorption, float3 scattering, float pathLength)
{
    return WaterEvaluatePreparedTransport(WaterPrepareOpticalMedium(absorption, scattering), pathLength);
}

#endif
