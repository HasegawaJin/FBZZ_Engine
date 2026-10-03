/// @file    RayMaterial.hlsli
/// @brief   Canonical PBR textures and candidate opacity with explicit ray LOD.
/// @author  Hasegawa Jin
/// @date    2026-10-01
#ifndef FBZZ_RAY_MATERIAL_HLSLI
#define FBZZ_RAY_MATERIAL_HLSLI
#ifndef COLOR_HLSLI
#include "Common/Color.hlsli"
#endif
#ifndef SPACE_HLSLI
#include "Common/Space.hlsli"
#endif
#ifndef MATH_HLSLI
#include "Common/Math.hlsli"
#endif

/// @note Matches C++ RaySurfaceRecord 128B. Encoded UNORM albedo/emission are decoded once, like Raster PBR.
struct RaySurfaceRecord {
    float4 baseColor;
    float3 emission; float metallic;
    float roughness; uint supported; float transmission; float ior;
    float3 attenuationColor; float attenuationDistance;
    float2 uvTiling; float2 uvOffset;
    float normalStrength; float alphaCutoff; float explicitTextureLod; uint textureMask;
    uint textureSrv[5]; uint dielectricFlags; float occlusionStrength; uint reserved;
};
#ifndef FBZZ_RAY_LINEAR_SAMPLER_DEFINED
#define FBZZ_RAY_LINEAR_SAMPLER_DEFINED
SamplerState rayLinearSampler : register(s0);
#endif

/// @note Vertex layout is position@0, normal@12, tangent@24, uv@36 (60B); indices are local to firstVertex.
float2 RayLoadUv(uint vertexSrv, uint stride, uint firstVertex, uint3 indices, float2 bary)
{
    ByteAddressBuffer vertices = ResourceDescriptorHeap[NonUniformResourceIndex(vertexSrv)];
    uint3 addresses = (firstVertex + indices) * stride + 36u;
    return asfloat(vertices.Load2(addresses.x)) * (1 - bary.x - bary.y)
        + asfloat(vertices.Load2(addresses.y)) * bary.x + asfloat(vertices.Load2(addresses.z)) * bary.y;
}
/// @note Transform and normalize each vertex tangent by objectToWorld, then barycentrically interpolate, like PBR VS.
float3 RayLoadTangent(uint vertexSrv, uint stride, uint firstVertex, uint3 indices, float2 bary, float3x3 objectToWorld)
{
    ByteAddressBuffer vertices = ResourceDescriptorHeap[NonUniformResourceIndex(vertexSrv)];
    uint3 addresses = (firstVertex + indices) * stride + 24u;
    float3 t0 = SafeNormalize(mul(objectToWorld, asfloat(vertices.Load3(addresses.x))), float3(1, 0, 0));
    float3 t1 = SafeNormalize(mul(objectToWorld, asfloat(vertices.Load3(addresses.y))), float3(1, 0, 0));
    float3 t2 = SafeNormalize(mul(objectToWorld, asfloat(vertices.Load3(addresses.z))), float3(1, 0, 0));
    return SafeNormalize(t0 * (1 - bary.x - bary.y) + t1 * bary.x + t2 * bary.y, float3(1, 0, 0));
}
/// @note SampleLevel 0 is explicit initial filtering, not a replacement for Raster derivatives or ray footprints.
/// @see https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-samplelevel Explicit texture LOD.
float4 RayMaterialTexture(RaySurfaceRecord surface, uint slot, float2 uv)
{
    if ((surface.textureMask & (1u << slot)) == 0) return 1;
    if (surface.textureSrv[slot] == 0xFFFFFFFFu) return asfloat(0x7FC00000u);
    Texture2D<float4> texture = ResourceDescriptorHeap[NonUniformResourceIndex(surface.textureSrv[slot])];
    return texture.SampleLevel(rayLinearSampler, uv * surface.uvTiling + surface.uvOffset, surface.explicitTextureLod);
}
/// @note clip(alpha-cutoff) keeps equality. Call before CommitNonOpaqueTriangleHit; do not discard an automatic opaque commit.
/// @see https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html#rayquery Nonopaque triangle candidates.
float RayMaterialOpacity(RaySurfaceRecord surface, float2 uv)
{
    return RayMaterialTexture(surface, 0u, uv).a * surface.baseColor.a;
}
/// @note NEE and committed hits share the same linear emission at explicit LOD; a full-triangle area PDF includes alpha-hole null samples.
/// @see https://pbr-book.org/4ed/Light_Sources/Area_Lights Spatially varying emitted radiance
float3 RayMaterialEmission(RaySurfaceRecord surface, float2 uv)
{
    return SRGBToLinear(RayMaterialTexture(surface, 3u, uv).rgb) * surface.emission;
}
/// @note AO is returned separately; Reference transport must not multiply this Raster ambient approximation into RAW radiance.
/// @note Emitter selection is a full-triangle PMF; UV-dependent radiance does not change the conditional uniform-area PDF.
/// @see Assets/Shaders/Material/Surface/PBR.hlsl PSMain UV/tint, normal strength and glTF G/B replacement.
bool RayMaterialEvaluate(RaySurfaceRecord surface, float2 uv, float3 worldNormal, float3 worldTangent,
    out RaySurfaceRecord evaluated, out float3 shadingNormal, out float ambientOcclusion)
{
    evaluated = surface;
    float4 albedoTexture = RayMaterialTexture(surface, 0u, uv);
    evaluated.baseColor = float4(SRGBToLinear(albedoTexture.rgb) * surface.baseColor.rgb,
        albedoTexture.a * surface.baseColor.a);
    shadingNormal = SafeNormalize(worldNormal, float3(0, 1, 0));
    if ((surface.textureMask & 2u) != 0) {
        float3 mapped = ApplyNormalMap(RayMaterialTexture(surface, 1u, uv).rgb,
            shadingNormal, SafeNormalize(worldTangent, float3(1, 0, 0)));
        shadingNormal = SafeNormalize(lerp(shadingNormal, mapped, saturate(surface.normalStrength)), shadingNormal);
    }
    if ((surface.textureMask & 4u) != 0) {
        float4 mr = RayMaterialTexture(surface, 2u, uv);
        evaluated.metallic = saturate(mr.b);
        evaluated.roughness = max(saturate(mr.g), 0.045f);
    }
    evaluated.emission = RayMaterialEmission(surface, uv);
    ambientOcclusion = lerp(1, RayMaterialTexture(surface, 4u, uv).r, surface.occlusionStrength);
    return all(isfinite(evaluated.baseColor)) && all(isfinite(evaluated.emission))
        && isfinite(evaluated.metallic) && isfinite(evaluated.roughness)
        && all(isfinite(shadingNormal)) && isfinite(ambientOcclusion);
}
#endif
