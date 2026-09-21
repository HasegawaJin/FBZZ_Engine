/// @file    FiberMaterialSettings.hpp
/// @brief   Fiber の material 値を有限範囲へ解決する GPU 共通レイアウト。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Graphics/Effects/FiberMaterialSettings.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>

namespace fbzz::asset {
struct MaterialAsset;

using renderer::FiberMaterialSettings;
/// @brief fiberMask を置く .mat の textures キー。シェーダーの tex5Index と対になり、Inspector には «tex5» の枠で出る。
inline constexpr const char* FIBER_MASK_TEXTURE_KEY = "tex5";

/// @return 欠損 / 非有限値は既定値、範囲外は clamp。asset が null なら既定の毛皮。
[[nodiscard]] FiberMaterialSettings ResolveFiberMaterial(const MaterialAsset* asset);

} /// @note namespace fbzz::asset
