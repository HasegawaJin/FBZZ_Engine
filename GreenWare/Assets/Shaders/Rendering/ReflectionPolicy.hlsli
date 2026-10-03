/// @file    ReflectionPolicy.hlsli
/// @brief   Hybrid reflection provider quality rules shared by tracing and resolve.
/// @author  Hasegawa Jin
/// @date    2026-10-01
#ifndef REFLECTION_POLICY_HLSLI
#define REFLECTION_POLICY_HLSLI

/// @brief Select the glossy metallic receivers that prefer available ray tracing.
/// @note Roughness is the filtered GBuffer value, not the authored value; strong-metal SSR source radiance remains view dependent.
/// @see https://jcgt.org/published/0003/04/04/paper.pdf Section 1: screen-space information and fallback limitations.
bool HybridReflectionPrefersRay(float materialMetallic, float materialRoughness)
{
    return isfinite(materialMetallic) && isfinite(materialRoughness)
        && materialMetallic >= 0.9f && materialRoughness <= 0.25f;
}

#endif /// @note REFLECTION_POLICY_HLSLI
