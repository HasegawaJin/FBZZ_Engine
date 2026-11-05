/// @file    LeafWetness.hlsli
/// @brief   Material-controlled wet reflection for leaf and needle surfaces.
/// @author  Hasegawa Jin
/// @date    2026-11-05
#ifndef FBZZ_LEAF_WETNESS_HLSLI
#define FBZZ_LEAF_WETNESS_HLSLI

/// @note PBR tex7 stores tangent-space slopes in RG, retained water in B, and bead coverage in A.
/// @note Thin leaves keep a broad highlight. Beads carry the narrower reflection, with no standing water.
WetSurface ApplyLeafWetness(float3 albedo, float roughness, float4 profile,
                            float3 tangent, inout float3 normal)
{
    const float film = saturate(weatherWetness) * saturate(profile.b);
    const float beads = saturate(profile.a);
    const float wetRoughness = lerp(0.66f, 0.39f, beads);
    WetSurface surface;
    surface.albedo = albedo * (1.0f - saturate(weatherDarkening) * film * 0.35f);
    surface.roughness = lerp(roughness, min(roughness, wetRoughness), film);

    const float3 projectedTangent = tangent - normal * dot(normal, tangent);
    const float tangentLengthSq = dot(projectedTangent, projectedTangent);
    if (tangentLengthSq > 1.0e-8f)
    {
        const float3 T = projectedTangent * rsqrt(tangentLengthSq);
        const float2 slopes = (profile.rg * 2.0f - 1.0f) * film;
        normal = normalize(normal + T * slopes.x + cross(normal, T) * slopes.y);
    }
    return surface;
}

#endif
