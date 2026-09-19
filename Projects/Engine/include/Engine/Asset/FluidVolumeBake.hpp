/// @file    FluidVolumeBake.hpp
/// @brief   .fluid → Volume Flipbook Baker の設定の写し (と、パネルで触った値の書き戻し)
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// 人 (Volume Flipbook Bake パネル) も AI (fluid.bake) も、流体の 3D 焼きはここを通す。
/// 別々に組むと、同じ .fluid から違う絵が焼ける。
#pragma once

#include <Fluid/FluidRecipe.hpp>
#include <Engine/Asset/VolumeFlipbookBaker.hpp>

#include <string>

namespace fbzz::asset {

/// recipe.bake と [output] / [render] から焼き設定を組む。
/// コマ数 = columns × rows、コマ間隔 = duration ÷ コマ数、タイル = frameSize、出力先 = .fluid の隣 (stem 名)。
/// Albedo / Emission Ramp はレシピの見た目 (液体の色・炎) から導く。
[[nodiscard]] VolumeFlipbookBakeSettings MakeVolumeBakeSettings(const fluid::FluidRecipe& recipe,
                                                                const std::string& fluidRecipePath);

/// MakeVolumeBakeSettings の逆向き。recipe.bake に載る流体固有の値だけを書き戻す
/// (コマ数やタイルは [output] の値を触らない)。
void StoreVolumeBakeSettings(const VolumeFlipbookBakeSettings& settings, fluid::FluidRecipe& recipe);

} // namespace fbzz::asset
