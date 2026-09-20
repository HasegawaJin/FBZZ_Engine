/// @file    FiberMaterialSettings.cpp
/// @brief   ファイル編集由来の不正値を GPU 転送前に取り除く。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <Engine/Asset/FiberMaterialSettings.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::asset {
namespace {
float ReadFiberFloat(const MaterialAsset* asset, const char* name, float fallback, float minimum, float maximum)
{
    if (!asset) return fallback;
    const auto it = asset->params.find(name);
    if (it == asset->params.end() || it->second.size() != 1 || !std::isfinite(it->second[0]))
        return fallback;
    return std::clamp(it->second[0], minimum, maximum);
}

math::Vector4 ReadFiberColor(const MaterialAsset* asset, const char* name, math::Vector4 fallback)
{
    if (!asset) return fallback;
    const auto it = asset->params.find(name);
    if (it == asset->params.end() || (it->second.size() != 3 && it->second.size() != 4)) return fallback;
    for (float value : it->second) if (!std::isfinite(value)) return fallback;
    return { std::clamp(it->second[0], 0.0f, 1.0f), std::clamp(it->second[1], 0.0f, 1.0f),
             std::clamp(it->second[2], 0.0f, 1.0f), 1.0f };
}
} // namespace

FiberMaterialSettings ResolveFiberMaterial(const MaterialAsset* asset)
{
    FiberMaterialSettings result;
    result.m_rootColor = ReadFiberColor(asset, "rootColor", result.m_rootColor);
    result.m_tipColor = ReadFiberColor(asset, "tipColor", result.m_tipColor);
    result.m_length = ReadFiberFloat(asset, "fiberLength", result.m_length, 0.0f, 2.0f);
    result.m_density = ReadFiberFloat(asset, "fiberDensity", result.m_density, 0.0f, 1.0f);
    result.m_thickness = ReadFiberFloat(asset, "fiberThickness", result.m_thickness, 0.001f, 0.49f);
    result.m_taper = ReadFiberFloat(asset, "fiberTaper", result.m_taper, 0.0f, 1.0f);
    result.m_frequency = ReadFiberFloat(asset, "fiberFrequency", result.m_frequency, 1.0f, 2048.0f);
    result.m_windResponse = ReadFiberFloat(asset, "windResponse", result.m_windResponse, 0.0f, 2.0f);
    result.m_maxBend = ReadFiberFloat(asset, "maxBend", result.m_maxBend, 0.0f, 2.0f);
    result.m_gravityBend = ReadFiberFloat(asset, "gravityBend", result.m_gravityBend, 0.0f, 2.0f);
    result.m_roughness = ReadFiberFloat(asset, "roughness", result.m_roughness, 0.02f, 1.0f);
    result.m_specular = ReadFiberFloat(asset, "specularStrength", result.m_specular, 0.0f, 1.0f);
    result.m_transmission = ReadFiberFloat(asset, "transmission", result.m_transmission, 0.0f, 1.0f);
    result.m_rootOcclusion = ReadFiberFloat(asset, "rootOcclusion", result.m_rootOcclusion, 0.0f, 1.0f);
    result.m_grass = ReadFiberFloat(asset, "grassShading", result.m_grass, 0.0f, 1.0f);
    result.m_worldMapping = ReadFiberFloat(asset, "worldMapping", result.m_worldMapping, 0.0f, 1.0f);
    result.m_finWidth = ReadFiberFloat(asset, "finSilhouetteWidth", result.m_finWidth, 0.01f, 1.0f);
    result.m_specularShift = ReadFiberFloat(asset, "specularShift", result.m_specularShift, 0.0f, 0.5f);
    result.m_secondarySpecular = ReadFiberFloat(asset, "secondarySpecular", result.m_secondarySpecular, 0.0f, 2.0f);
    result.m_colorVariation = ReadFiberFloat(asset, "colorVariation", result.m_colorVariation, 0.0f, 1.0f);
    result.m_clumping = ReadFiberFloat(asset, "clumping", result.m_clumping, 0.0f, 1.0f);
    result.m_clumpTwist = ReadFiberFloat(asset, "clumpTwist", result.m_clumpTwist, -2.0f, 2.0f);
    return result;
}
} // namespace fbzz::asset
