/// @file    HybridGlassPolicy.hlsli
/// @brief   Typed Hybrid receiver markers, reflection result kinds and resolve stages.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#ifndef FBZZ_HYBRID_GLASS_POLICY_HLSLI
#define FBZZ_HYBRID_GLASS_POLICY_HLSLI

static const float HYBRID_REFLECTION_RESOLVE_FINAL = 1.0f;
static const float HYBRID_REFLECTION_RESOLVE_SOURCE = 2.0f;
static const float HYBRID_REFLECTION_OPAQUE_SPECULAR = 1.0f;
static const float HYBRID_REFLECTION_DIELECTRIC_FULL = 2.0f;
static const float HYBRID_GLASS_SUPPORTED = 1.0f;
/// @note Marker2 also denotes authored but unsupported transmission; neither marker may be sampled as an opaque SSR source.
bool HybridGlassReceiver(float marker) { return isfinite(marker) && marker > 0.5f; }
/// @note Target2 alpha keeps the Hybrid marker in the integer part and inverse material AO in the low quarter.
/// @note Existing 0/1/2 markers decode to full AO and remain valid in the reflection classifiers.
float PackGBufferSurface(float marker, float materialAO) { return marker + 0.25f * (1.0f - saturate(materialAO)); }
float GBufferSurfaceMarker(float packedSurface) { return floor(packedSurface); }
float GBufferMaterialAO(float packedSurface) { return saturate(1.0f - 4.0f * frac(packedSurface)); }

bool HybridReflectionResolveActive(float stage)
{
    return stage == HYBRID_REFLECTION_RESOLVE_FINAL || stage == HYBRID_REFLECTION_RESOLVE_SOURCE;
}

#endif /// @note FBZZ_HYBRID_GLASS_POLICY_HLSLI
