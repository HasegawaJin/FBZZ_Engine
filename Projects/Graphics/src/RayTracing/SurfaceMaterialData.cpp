/// @file    SurfaceMaterialData.cpp
/// @brief   正規化された GBuffer 材質から定数 PBR 表面と厳密比較可能な GPU 入力を解決する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <Graphics/RayTracing/SurfaceMaterialData.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace fbzz::renderer {
namespace {

bool FiniteSurface(const SurfaceMaterialData& material)
{
    return std::isfinite(material.baseColor.x) && std::isfinite(material.baseColor.y)
        && std::isfinite(material.baseColor.z) && std::isfinite(material.baseColor.w)
        && std::isfinite(material.emission.x) && std::isfinite(material.emission.y)
        && std::isfinite(material.emission.z) && std::isfinite(material.metallic)
        && std::isfinite(material.roughness)
        && std::isfinite(material.dielectric.transmission) && std::isfinite(material.dielectric.ior)
        && std::isfinite(material.dielectric.attenuationColor.x)
        && std::isfinite(material.dielectric.attenuationColor.y)
        && std::isfinite(material.dielectric.attenuationColor.z)
        && std::isfinite(material.dielectric.attenuationDistance)
        && std::isfinite(material.uvTiling[0]) && std::isfinite(material.uvTiling[1])
        && std::isfinite(material.uvOffset[0]) && std::isfinite(material.uvOffset[1])
        && std::isfinite(material.normalStrength) && std::isfinite(material.occlusionStrength)
        && std::isfinite(material.alphaCutoff) && std::isfinite(material.explicitTextureLod);
}

bool ValidDielectricSettings(const SolidDielectricSettings& value)
{
    return value.transmission >= 0 && value.transmission <= 1 && value.ior > 0
        && value.attenuationDistance > 0
        && value.attenuationColor.x >= 0 && value.attenuationColor.x <= 1
        && value.attenuationColor.y >= 0 && value.attenuationColor.y <= 1
        && value.attenuationColor.z >= 0 && value.attenuationColor.z <= 1;
}

bool SupportedDielectric(const SurfaceMaterialData& material)
{
    return material.dielectric.transmission == 1
        && material.metallic == 0 && material.roughness >= 0 && material.roughness <= 1
        && (!material.dielectric.thinWalled || material.roughness == 0) && material.baseColor.w == 1
        && (!material.dielectric.thinWalled || (material.dielectric.attenuationColor.x == 1
            && material.dielectric.attenuationColor.y == 1 && material.dielectric.attenuationColor.z == 1))
        && material.textureMask == 0
        && material.emission.x == 0 && material.emission.y == 0 && material.emission.z == 0;
}

bool EqualFloat(float left, float right)
{
    return std::bit_cast<uint32_t>(left) == std::bit_cast<uint32_t>(right);
}

} /// @note namespace

bool SolidDielectricSettings::operator==(const SolidDielectricSettings& other) const
{
    return EqualFloat(transmission, other.transmission) && EqualFloat(ior, other.ior)
        && EqualFloat(attenuationColor.x, other.attenuationColor.x)
        && EqualFloat(attenuationColor.y, other.attenuationColor.y)
        && EqualFloat(attenuationColor.z, other.attenuationColor.z)
        && EqualFloat(attenuationDistance, other.attenuationDistance) && thinWalled == other.thinWalled;
}

bool SurfaceMaterialData::operator==(const SurfaceMaterialData& other) const
{
    return EqualFloat(baseColor.x, other.baseColor.x) && EqualFloat(baseColor.y, other.baseColor.y)
        && EqualFloat(baseColor.z, other.baseColor.z) && EqualFloat(baseColor.w, other.baseColor.w)
        && EqualFloat(emission.x, other.emission.x) && EqualFloat(emission.y, other.emission.y)
        && EqualFloat(emission.z, other.emission.z) && EqualFloat(metallic, other.metallic)
        && EqualFloat(roughness, other.roughness)
        && standardSurfaceSupported == other.standardSurfaceSupported && issue == other.issue
        && dielectric == other.dielectric && solidDielectricSupported == other.solidDielectricSupported
        && textures == other.textures && textureMask == other.textureMask
        && EqualFloat(uvTiling[0], other.uvTiling[0]) && EqualFloat(uvTiling[1], other.uvTiling[1])
        && EqualFloat(uvOffset[0], other.uvOffset[0]) && EqualFloat(uvOffset[1], other.uvOffset[1])
        && EqualFloat(normalStrength, other.normalStrength) && EqualFloat(occlusionStrength, other.occlusionStrength)
        && EqualFloat(alphaCutoff, other.alphaCutoff) && EqualFloat(explicitTextureLod, other.explicitTextureLod);
}

SurfaceMaterialData ResolveConstantSurfaceMaterial(const std::array<uint8_t, 96>& params,
    bool standardPbr, bool usesTextures, bool advancedLobes, const SolidDielectricSettings& dielectric)
{
    auto result = ResolveTexturedSurfaceMaterial(params, standardPbr, {}, usesTextures, advancedLobes, dielectric);
    if (standardPbr && (result.textureMask != 0 || usesTextures)) {
        result.issue = SurfaceMaterialIssue::TEXTURE_UNSUPPORTED;
        result.standardSurfaceSupported = result.solidDielectricSupported = false;
    }
    return result;
}

SurfaceMaterialData ResolveTexturedSurfaceMaterial(const std::array<uint8_t, 96>& params,
    bool standardPbr, const std::array<RayTextureBinding, 5>& textures, bool unresolvedTexture,
    bool advancedLobes, const SolidDielectricSettings& dielectric)
{
    const auto read = [&](uint32_t offset) {
        float value = 0;
        std::memcpy(&value, params.data() + offset, sizeof(value));
        return value;
    };
    SurfaceMaterialData result;
    result.dielectric = dielectric;
    result.baseColor = {read(0), read(4), read(8), read(12)};
    result.metallic = read(16);
    result.roughness = read(20);
    result.normalStrength = read(24);
    result.occlusionStrength = read(28);
    result.uvTiling = {read(48), read(52)};
    result.uvOffset = {read(56), read(60)};
    result.alphaCutoff = read(64);
    result.textures = textures;
    const math::Vector3 emissionColor{read(32), read(36), read(40)};
    const float emissionScale = read(44);
    result.emission = emissionColor * emissionScale;
    uint32_t textureMask = 0;
    std::memcpy(&textureMask, params.data() + 80, sizeof(textureMask));
    result.textureMask = textureMask;
    bool unavailable = false;
    for (uint32_t i = 0; i < textures.size(); ++i)
        if ((textureMask & (1u << i)) && (!textures[i].texture || textures[i].contentVersion == 0)) unavailable = true;
    const bool valid = FiniteSurface(result) && std::isfinite(emissionScale)
        && std::isfinite(emissionColor.x) && std::isfinite(emissionColor.y) && std::isfinite(emissionColor.z)
        && result.baseColor.x >= 0 && result.baseColor.y >= 0 && result.baseColor.z >= 0
        && result.baseColor.w >= 0 && emissionColor.x >= 0 && emissionColor.y >= 0
        && emissionColor.z >= 0 && emissionScale >= 0 && ValidDielectricSettings(dielectric);
    /// @note 材質 clamp のみ共有する。RT に画面微分や濡れを暗黙に適用しない。
    /// @see Assets/Shaders/Material/Surface/PBR.hlsl PSMain の metallic / roughness と emission。
    if (valid && dielectric.transmission == 0) {
        result.metallic = std::clamp(result.metallic, 0.0f, 1.0f);
        result.roughness = std::clamp(result.roughness, 0.045f, 1.0f);
    }
    result.issue = !standardPbr ? SurfaceMaterialIssue::UNSUPPORTED_SHADER
        : unresolvedTexture || (textureMask & ~31u) != 0 ? SurfaceMaterialIssue::TEXTURE_UNSUPPORTED
        : unavailable ? SurfaceMaterialIssue::TEXTURE_UNAVAILABLE
        : advancedLobes ? SurfaceMaterialIssue::ADVANCED_LOBES_UNSUPPORTED
        : !valid ? SurfaceMaterialIssue::INVALID_CONSTANTS
        : dielectric.transmission != 0 && !SupportedDielectric(result)
            ? SurfaceMaterialIssue::SOLID_DIELECTRIC_UNSUPPORTED : SurfaceMaterialIssue::NONE;
    result.standardSurfaceSupported = result.issue == SurfaceMaterialIssue::NONE && dielectric.transmission == 0;
    result.solidDielectricSupported = result.issue == SurfaceMaterialIssue::NONE && dielectric.transmission == 1;
    return result;
}

RaySurfaceRecord MakeRaySurfaceRecord(const SurfaceMaterialData& material)
{
    RaySurfaceRecord result;
    if (!FiniteSurface(material)) return result;
    result.baseColor = {material.baseColor.x, material.baseColor.y, material.baseColor.z, material.baseColor.w};
    result.emission = {material.emission.x, material.emission.y, material.emission.z};
    result.metallic = material.metallic;
    result.roughness = material.roughness;
    result.transmission = material.dielectric.transmission;
    result.ior = material.dielectric.ior;
    result.attenuationColor = {material.dielectric.attenuationColor.x,
        material.dielectric.attenuationColor.y, material.dielectric.attenuationColor.z};
    result.attenuationDistance = material.dielectric.attenuationDistance;
    result.uvTiling = material.uvTiling;
    result.uvOffset = material.uvOffset;
    result.normalStrength = material.normalStrength;
    result.occlusionStrength = material.occlusionStrength;
    result.alphaCutoff = material.alphaCutoff;
    result.explicitTextureLod = material.explicitTextureLod;
    result.textureMask = material.textureMask;
    result.dielectricFlags = material.dielectric.thinWalled ? 1u : 0u;
    if (material.issue == SurfaceMaterialIssue::NONE && ValidDielectricSettings(material.dielectric)) {
        if (material.standardSurfaceSupported && material.dielectric.transmission == 0) result.supported = 1;
        else if (material.solidDielectricSupported && SupportedDielectric(material)) result.supported = 2;
    }
    return result;
}

bool IsHybridRayDielectricSupported(const SurfaceMaterialData& material)
{
    return MakeRaySurfaceRecord(material).supported == 2u;
}

float ResolveHybridRaySurfaceMarker(const SurfaceMaterialData& material)
{
    if (material.dielectric.transmission == 0.0f) return 0.0f;
    return IsHybridRayDielectricSupported(material) ? 1.0f : 2.0f;
}

std::array<uint8_t, 96> MakeHybridGBufferParams(const std::array<uint8_t, 96>& canonicalParams,
    const SurfaceMaterialData& material)
{
    auto params = canonicalParams;
    const float marker = ResolveHybridRaySurfaceMarker(material);
    std::memcpy(params.data() + 84, &marker, sizeof(marker));
    return params;
}

const char* DescribeSurfaceMaterialIssue(SurfaceMaterialIssue issue)
{
    switch (issue) {
    case SurfaceMaterialIssue::NONE: return "Constant standard PBR supported";
    case SurfaceMaterialIssue::UNSUPPORTED_SHADER: return "Hit surface is not canonical standard PBR";
    case SurfaceMaterialIssue::TEXTURE_UNSUPPORTED: return "Textured hit surface unsupported";
    case SurfaceMaterialIssue::ADVANCED_LOBES_UNSUPPORTED: return "Clearcoat / sheen / anisotropy hit surface unsupported";
    case SurfaceMaterialIssue::INVALID_CONSTANTS: return "Nonfinite or negative surface constants";
    case SurfaceMaterialIssue::SOLID_DIELECTRIC_UNSUPPORTED: return "Dielectric requires nonemissive full coverage; rough thin sheets unsupported";
    case SurfaceMaterialIssue::TEXTURE_UNAVAILABLE: return "Ray material texture or stable content version unavailable";
    case SurfaceMaterialIssue::TEXTURED_EMISSION_UNSUPPORTED: return "Textured emission distribution unsupported";
    }
    return "Unknown surface material issue";
}

} /// @note namespace fbzz::renderer
