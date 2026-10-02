/// @file    RayReflectionReconstruction.cs.hlsl
/// @brief   Actual-terminal camera-motion validation and edge-aware HDR reflection reconstruction.
/// @author  Hasegawa Jin
/// @date    2026-10-01
/// @note Content changes reset globally. Camera motion requires a planar reflected terminal or a proven direct thin transmission; receiver velocity never substitutes for transport motion.
/// @see https://research.nvidia.com/labs/rtr/publication/schied2017spatiotemporal/ Edge-aware temporal/spatial validation; not a complete SVGF implementation
#include "Common/BindlessIndices.hlsli"
#include "RayTracing/RayMaterial.hlsli"
#include "RayTracing/RayDielectricInterface.hlsli"
#include "RayTracing/RayReflectionMotion.hlsli"

cbuffer RayReflectionReconstructionConstants : register(b0)
{
    float4 cameraPosition, cameraRight, cameraUp, cameraForward;
    uint width, height, resetHistory, stage;
    float previousJitterX, previousJitterY, currentJitterX, currentJitterY;
    uint historyLimit, spatialRadius, hasGBuffer, temporalAllowed;
    float4 previousCameraPosition, previousCameraRight, previousCameraUp, previousCameraForward;
    float4 constantEnvironmentRadiance;
    float nearDistance, farDistance; uint cameraMotion, movingHistoryLimit;
};
struct RayReflectionHistoryRecord
{
    float4 radianceCount;
};

bool ValidSurface(RayReflectionSurface surface)
{
    return (surface.objectMaterialValid.w == 1u || surface.objectMaterialValid.w == 2u)
        && all(isfinite(surface.positionDepth))
        && all(isfinite(surface.normalRoughness)) && all(isfinite(surface.geometricNormalOffset))
        && surface.positionDepth.w > 0 && surface.normalRoughness.w >= 0 && surface.normalRoughness.w <= 1
        && surface.geometricNormalOffset.w >= 0
        && abs(dot(surface.normalRoughness.xyz, surface.normalRoughness.xyz) - 1) < 0.01f
        && abs(dot(surface.geometricNormalOffset.xyz, surface.geometricNormalOffset.xyz) - 1) < 0.01f;
}

bool SameSurface(RayReflectionSurface current, RayReflectionSurface other, bool temporal)
{
    if (!ValidSurface(current) || !ValidSurface(other)
        || any(current.objectMaterialValid != other.objectMaterialValid)
        || dot(current.normalRoughness.xyz, other.normalRoughness.xyz) < 0.98f
        || dot(current.geometricNormalOffset.xyz, other.geometricNormalOffset.xyz) < 0.98f
        || abs(current.normalRoughness.w - other.normalRoughness.w) > 0.03f) return false;
    float3 delta = current.positionDepth.xyz - other.positionDepth.xyz;
    float3 magnitude = max(abs(current.positionDepth.xyz), abs(other.positionDepth.xyz));
    /// @note Plane residuals accept neighbouring points on one plane, unlike Euclidean world-position cutoffs.
    /// @see https://pbr-book.org/4ed/Shapes/Managing_Rounding_Error Floating-point error bounds and geometric offsets
    float roundoff = 8.0f * 1.192092896e-7f * max(max(magnitude.x, magnitude.y), magnitude.z);
    float tolerance = current.geometricNormalOffset.w + other.geometricNormalOffset.w + roundoff;
    float normalDifference = length(current.geometricNormalOffset.xyz - other.geometricNormalOffset.xyz);
    float curvatureAllowance = length(delta) * normalDifference;
    if (!isfinite(curvatureAllowance) || !isfinite(tolerance)
        || abs(dot(delta, current.geometricNormalOffset.xyz)) > tolerance + curvatureAllowance
        || abs(dot(delta, other.geometricNormalOffset.xyz)) > tolerance + curvatureAllowance) return false;
    if (temporal) {
        float projectionScale = cameraPosition.w != 0 ? 1.0f : max(current.positionDepth.w, other.positionDepth.w);
        float2 jitterDifference = abs(float2(currentJitterX - previousJitterX, currentJitterY - previousJitterY));
        float jitterFootprint = projectionScale * length(jitterDifference * float2(cameraRight.w, cameraUp.w));
        float3 incident = cameraPosition.w != 0 ? cameraForward.xyz : normalize(current.positionDepth.xyz - cameraPosition.xyz);
        float incidence = abs(dot(current.geometricNormalOffset.xyz, incident));
        if (!isfinite(jitterFootprint) || !isfinite(incidence) || incidence <= 1e-4f
            || length(delta) > jitterFootprint / incidence + tolerance + curvatureAllowance) return false;
    }
    return true;
}

bool ValidRadiance(float4 raw)
{
    return (raw.a == 1.0f || raw.a == 2.0f) && all(isfinite(raw)) && all(raw.rgb >= 0);
}

bool ValidSample(float4 raw, RayReflectionSurface surface)
{
    /// @note Whole dielectric radiance and opaque specular radiance use disjoint history kinds; metadata alone cannot validate a raw sample.
    return ValidRadiance(raw) && ValidSurface(surface) && raw.a == float(surface.objectMaterialValid.w);
}

bool MotionEligible(RayReflectionSurface surface)
{
    uint kind = surface.motion.objectPrimitiveKind.w;
    return ValidSurface(surface) && all(isfinite(surface.motion.terminalPositionParameter))
        && dot(surface.normalRoughness.xyz, surface.geometricNormalOffset.xyz) > 0.99999f
        && ((kind == 1u && surface.objectMaterialValid.w == 1u && surface.normalRoughness.w <= 0.05f
                && constantEnvironmentRadiance.w == 1 && all(constantEnvironmentRadiance.rgb == 0))
            || (kind == 2u && surface.objectMaterialValid.w == 2u && surface.normalRoughness.w == 0
                && surface.motion.terminalPositionParameter.w > 0 && constantEnvironmentRadiance.w == 1));
}

float3 TransportPoint(RayReflectionSurface surface)
{
    float3 terminal = surface.motion.terminalPositionParameter.xyz;
    if (surface.motion.objectPrimitiveKind.w == 1u) {
        float3 normal = surface.geometricNormalOffset.xyz;
        terminal -= 2 * dot(terminal - surface.positionDepth.xyz, normal) * normal;
    }
    return terminal;
}

/// @note Reprojection uses a mirror's virtual terminal, or a thin sheet's actual straight-transmission terminal, not its receiver position.
/// @see https://github.com/NVIDIA-RTX/NRD/blob/master/README.md Primary Surface Replacement: planar virtual-space motion and curved-surface limitations
bool ProjectTerminal(float3 terminalPosition, bool previous, out float2 pixel)
{
    float4 position = previous ? previousCameraPosition : cameraPosition;
    float4 right = previous ? previousCameraRight : cameraRight;
    float4 up = previous ? previousCameraUp : cameraUp;
    float4 forward = previous ? previousCameraForward : cameraForward;
    float3 delta = terminalPosition - position.xyz;
    float z = dot(delta, forward.xyz);
    float scale = position.w != 0 ? 1 : z;
    float2 ndc = float2(dot(delta, right.xyz), dot(delta, up.xyz)) / (scale * float2(right.w, up.w));
    ndc += previous ? float2(previousJitterX, previousJitterY) : float2(currentJitterX, currentJitterY);
    pixel = float2((ndc.x + 1) * width, (1 - ndc.y) * height) * 0.5f;
    return z >= nearDistance && z <= farDistance && all(isfinite(pixel)) && scale > 0
        && all(pixel >= 0) && all(pixel < float2(width, height));
}

bool MovingSurfaceMatch(RayReflectionSurface current, RayReflectionSurface old, uint2 currentPixel, uint2 oldPixel)
{
    if (!MotionEligible(current) || !MotionEligible(old)
        || any(current.motion.objectPrimitiveKind != old.motion.objectPrimitiveKind)
        || current.motion.terminalPositionParameter.w != old.motion.terminalPositionParameter.w
        || !SameSurface(current, old, false)
        || dot(current.geometricNormalOffset.xyz, old.geometricNormalOffset.xyz) < 0.99999f) return false;
    float2 currentProjection, oldProjection, expectedProjection;
    if (!ProjectTerminal(TransportPoint(current), false, currentProjection)
        || !ProjectTerminal(TransportPoint(old), true, oldProjection)
        || !ProjectTerminal(TransportPoint(current), true, expectedProjection)) return false;
    /// @note Both guides must lie inside their own center-pixel footprint; an average terminal outside that footprint is not a motion proof.
    return all(abs(currentProjection - (float2(currentPixel) + 0.5f)) <= 0.5f)
        && all(abs(oldProjection - (float2(oldPixel) + 0.5f)) <= 0.5f)
        && all(abs(expectedProjection - oldProjection) <= 1.0f);
}

bool ThinSignal(float4 raw, RayReflectionSurface surface, out float3 signal, out float reflection)
{
    signal = raw.rgb; reflection = 0;
    if (!MotionEligible(surface) || surface.motion.objectPrimitiveKind.w != 2u) return false;
    float3 view = cameraPosition.w != 0 ? -cameraForward.xyz : normalize(cameraPosition.xyz - surface.positionDepth.xyz);
    reflection = RayThinReflectance(dot(surface.normalRoughness.xyz, view), surface.motion.terminalPositionParameter.w);
    float transmission = 1 - reflection;
    if (!isfinite(reflection) || transmission <= 1e-4f) return false;
    signal = (raw.rgb - reflection * constantEnvironmentRadiance.rgb) / transmission;
    /// @note Negative demodulation after half-output rounding rejects reuse rather than conditioning the estimator on surviving samples.
    return all(isfinite(signal)) && all(signal >= 0);
}

void Temporal(uint2 pixel)
{
    uint index = pixel.y * width + pixel.x;
    Texture2D<float4> raw = ResourceDescriptorHeap[FbzzPixelSlot(5)];
    StructuredBuffer<RayReflectionSurface> surfaces = ResourceDescriptorHeap[FbzzPixelSlot(14)];
    StructuredBuffer<RayReflectionSurface> previousSurfaces = ResourceDescriptorHeap[FbzzPixelSlot(15)];
    StructuredBuffer<RayReflectionHistoryRecord> previousHistory = ResourceDescriptorHeap[FbzzPixelSlot(18)];
    RWStructuredBuffer<RayReflectionHistoryRecord> history = ResourceDescriptorHeap[FbzzUavSlot(2)];
    RayReflectionHistoryRecord result = (RayReflectionHistoryRecord)0;
    RayReflectionSurface surface = surfaces[index];
    float4 current = raw.Load(int3(pixel, 0));
    if (ValidSample(current, surface)) {
        float3 signal; float reflection;
        bool thin = ThinSignal(current, surface, signal, reflection);
        if (!thin) signal = current.rgb;
        /// @note Negative count tags demodulated thin transmission; positive counts retain full RAW or opaque specular signal.
        result.radianceCount = float4(signal, thin ? -1 : 1);
        if (!resetHistory && temporalAllowed) {
            uint oldIndex = index;
            bool matched = false;
            if (cameraMotion) {
                float2 previousPixel;
                if (MotionEligible(surface) && ProjectTerminal(TransportPoint(surface), true, previousPixel)) {
                    uint2 oldPixel = uint2(previousPixel);
                    oldIndex = oldPixel.y * width + oldPixel.x;
                    matched = MovingSurfaceMatch(surface, previousSurfaces[oldIndex], pixel, oldPixel);
                }
            } else matched = SameSurface(surface, previousSurfaces[index], true)
                && (!thin || (all(surface.motion.objectPrimitiveKind == previousSurfaces[index].motion.objectPrimitiveKind)
                    && surface.motion.terminalPositionParameter.w == previousSurfaces[index].motion.terminalPositionParameter.w));
            /// @note Separate previous/current resources make arbitrary validated reprojection indices safe; no in-place inter-pixel read/write race is possible.
            /// @see https://learn.microsoft.com/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states UAV dispatch ordering
            RayReflectionHistoryRecord old = previousHistory[oldIndex];
            float count = abs(old.radianceCount.w);
            if (matched && all(isfinite(old.radianceCount)) && all(old.radianceCount.rgb >= 0)
                && count >= 1 && count <= 64 && ((old.radianceCount.w < 0) == thin)) {
                /// @note The configured limit bounds an initial arithmetic mean then an EMA; camera-motion reuse is a short angle-response approximation, not invariant outgoing radiance.
                uint limit = cameraMotion ? min(historyLimit, movingHistoryLimit) : historyLimit;
                float weight = min(count, clamp(limit, 1u, 64u) - 1u);
                float3 mean = old.radianceCount.rgb + (signal - old.radianceCount.rgb) / (weight + 1);
                result.radianceCount = float4(mean, thin ? -(weight + 1) : weight + 1);
            }
        }
    }
    /// @note Every pixel writes even on reset/invalid raw; failed reconstruction rejects partially updated history on the next frame.
    history[index] = result;
}

void Spatial(uint2 pixel)
{
    uint index = pixel.y * width + pixel.x;
    Texture2D<float4> raw = ResourceDescriptorHeap[FbzzPixelSlot(5)];
    StructuredBuffer<RayReflectionHistoryRecord> history = ResourceDescriptorHeap[FbzzPixelSlot(14)];
    StructuredBuffer<RayReflectionSurface> surfaces = ResourceDescriptorHeap[FbzzPixelSlot(15)];
    RWTexture2D<float4> output = ResourceDescriptorHeap[FbzzUavSlot(0)];
    float4 current = raw.Load(int3(pixel, 0));
    RayReflectionSurface center = surfaces[index];
    RayReflectionHistoryRecord accumulated = history[index];
    if (!ValidSample(current, center) || abs(accumulated.radianceCount.w) < 1) {
        output[pixel] = current;
        return;
    }
    float3 radiance = accumulated.radianceCount.rgb;
    if (accumulated.radianceCount.w < 0) {
        float3 signal; float reflection;
        if (!ThinSignal(current, center, signal, reflection)) { output[pixel] = current; return; }
        radiance = constantEnvironmentRadiance.rgb * reflection + radiance * (1 - reflection);
    }
    float totalWeight = 1;
    /// @note Glass never mixes neighbouring pixels; sharp mirrors remain spatially unfiltered even when their terminal correspondence permits temporal reuse.
    int radius = center.objectMaterialValid.w == 1u && center.normalRoughness.w >= 0.4f
        ? int(min(spatialRadius, 2u)) : 0;
    for (int y = -radius; y <= radius; ++y) for (int x = -radius; x <= radius; ++x) {
        int2 neighbourPixel = int2(pixel) + int2(x, y);
        if ((x == 0 && y == 0) || any(neighbourPixel < 0) || any(neighbourPixel >= int2(width, height))) continue;
        uint neighbourIndex = uint(neighbourPixel.y) * width + uint(neighbourPixel.x);
        RayReflectionHistoryRecord neighbour = history[neighbourIndex];
        if (!ValidSample(raw.Load(int3(neighbourPixel, 0)), surfaces[neighbourIndex]) || neighbour.radianceCount.w < 1
            || !all(isfinite(neighbour.radianceCount)) || !SameSurface(center, surfaces[neighbourIndex], false)) continue;
        if (hasGBuffer) {
            Texture2D<float4> albedoRoughness = ResourceDescriptorHeap[FbzzPixelSlot(16)];
            Texture2D<float4> normalMetallic = ResourceDescriptorHeap[FbzzPixelSlot(17)];
            if (any(abs(albedoRoughness.Load(int3(pixel, 0)) - albedoRoughness.Load(int3(neighbourPixel, 0))) > 0.02f)
                || abs(normalMetallic.Load(int3(pixel, 0)).a - normalMetallic.Load(int3(neighbourPixel, 0)).a) > 0.02f) continue;
        }
        float weight = 1.0f / (1 + x * x + y * y);
        radiance += neighbour.radianceCount.rgb * weight;
        totalWeight += weight;
    }
    radiance /= totalWeight;
    /// @note Half-output overflow falls back to current raw, not an infinite filtered value or stale history.
    output[pixel] = all(isfinite(radiance)) && all(radiance >= 0) && all(radiance <= 65504.0f)
        ? float4(radiance, current.a) : current;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 pixel : SV_DispatchThreadID)
{
    if (pixel.x >= width || pixel.y >= height) return;
    if (stage == 0) Temporal(pixel.xy); else Spatial(pixel.xy);
}
