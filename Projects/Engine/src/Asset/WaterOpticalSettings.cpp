/// @file    WaterOpticalSettings.cpp
/// @brief   本描画と材質プレビューで光学係数の既定と検証を共有する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <Engine/Asset/WaterOpticalSettings.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <algorithm>
#include <cmath>
namespace fbzz::asset {
namespace {
float Coefficient(float value)
{
    if (std::isnan(value) || value <= 0.0f) return 0.0f;
    return (std::min)(value, 1000.0f);
}
math::Vector3 ReadCoefficients(const MaterialAssetAuthoring* material, const char* key, math::Vector3 fallback)
{
    if (!material) return fallback;
    const auto found = material->params.find(key);
    if (found == material->params.end() || found->second.size() < 3) return fallback;
    return {Coefficient(found->second[0]), Coefficient(found->second[1]), Coefficient(found->second[2])};
}
float ReadFinite(const MaterialAssetAuthoring* material, const char* key, float fallback)
{
    if (!material) return fallback;
    const auto found = material->params.find(key);
    return found != material->params.end() && !found->second.empty() && std::isfinite(found->second[0])
        ? found->second[0] : fallback;
}
} /// @note namespace
void InitializeWaterAppearanceParameters(const MaterialAssetAuthoring& values,
    const renderer::ShaderDescriptor& descriptor, std::vector<uint8_t>& parameters)
{
    parameters.assign(descriptor.cbufferSize, uint8_t{0});
    InitDefaultMaterialParams(descriptor, parameters);
    /// @note Name-based writes preserve old missing-key appearance without assuming the user's byte offsets.
    static const std::unordered_map<std::string, std::vector<float>> defaults{
        {"rimGlowStrength", {0.40f}}, {"minShallowAlpha", {0.65f}}, {"specularStrength", {0.75f}},
        {"skyReflectTint", {0.45f, 0.82f, 1.0f}}, {"rippleRingColor", {0.88f, 0.97f, 1.0f}},
        {"rippleRingStrength", {0.72f}}
    };
    ApplyMaterialParamOverrides(defaults, descriptor, parameters);
    ApplyMaterialAssetParams(values, descriptor, parameters);
}
WaterOpticalSettings ResolveWaterOpticalSettings(const MaterialAssetAuthoring& values)
{
    const auto* material = &values;
    WaterOpticalSettings result;
    result.enabled = ReadFinite(material, "opticalMode", 0.0f) > 0.5f;
    result.absorption = ReadCoefficients(material, "absorptionCoefficients", result.absorption);
    result.scattering = ReadCoefficients(material, "scatteringCoefficients", result.scattering);
    result.phaseG = (std::clamp)(ReadFinite(material, "phaseG", 0.0f), -0.95f, 0.95f);
    return result;
}
WaterOpticalSettings ResolveWaterOpticalSettings(const MaterialAsset* material)
{
    const auto snapshot = material ? material->ReadAuthoring() : MaterialAsset::AuthoringSnapshotPtr{};
    return snapshot ? ResolveWaterOpticalSettings(snapshot->Data()) : WaterOpticalSettings{};
}
} /// @note namespace fbzz::asset
