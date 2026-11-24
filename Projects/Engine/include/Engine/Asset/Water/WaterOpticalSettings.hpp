/// @file    WaterOpticalSettings.hpp
/// @brief   水の光学係数を材質から共通の有限値へ解決する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#pragma once
#include <Engine/Api.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Graphics/Renderer/Shaders/ShaderDescriptor.hpp>
#include <cstdint>
#include <vector>
namespace fbzz::asset {
struct MaterialAsset;
struct MaterialAssetAuthoring;
/// @note 反射名で従来の水面の外観の既定値を解決してから、型付きの編集値を反映する。
/// @note b2 のレイアウトはシェーダーが所有する。水面の形状とビューの定数は b0/b1 に保持する。
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
/// @note 資源のコールバックをまたいで値を渡す間、呼び出し側が不変のスナップショットを所有する。
[[nodiscard]] FBZZ_ENGINE_API WaterOpticalSettings ResolveWaterOpticalSettings(const MaterialAssetAuthoring& values);
} /// @note namespace fbzz::asset
