/// @file    WaterOpticalSettings.hpp
/// @brief   水の光学係数を材質から共通の有限値へ解決する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Engine/Api.hpp>
#include <Math/Vector3.hpp>
#include <Graphics/Renderer/ShaderDescriptor.hpp>
#include <cstdint>
#include <vector>
namespace fbzz::asset {
struct MaterialAsset;
struct MaterialAssetAuthoring;
/// @note Resolve the legacy water appearance defaults by reflected name, then apply authored typed values.
/// @note The shader owns b2 layout; water geometry/view constants remain in b0/b1.
FBZZ_ENGINE_API void InitializeWaterAppearanceParameters(const MaterialAssetAuthoring& values,
    const renderer::ShaderDescriptor& descriptor, std::vector<uint8_t>& parameters);
struct WaterOpticalSettings {
    bool enabled = false;
    math::Vector3 absorption{0.35f, 0.06f, 0.02f};
    math::Vector3 scattering{0.02f, 0.035f, 0.045f};
    float phaseG = 0.0f;
};
/// @note RGB消散係数は [1/m]。NaN/負値は0、正の無限大は1000、位相は [-.95,.95] に制限する。
/// @see Docs/design/water-wave-optics-morph.md 光学係数。
[[nodiscard]] FBZZ_ENGINE_API WaterOpticalSettings ResolveWaterOpticalSettings(const MaterialAsset* material);
/// @note The caller owns the immutable snapshot when passing its values across resource callbacks.
[[nodiscard]] FBZZ_ENGINE_API WaterOpticalSettings ResolveWaterOpticalSettings(const MaterialAssetAuthoring& values);
} /// @note namespace fbzz::asset
