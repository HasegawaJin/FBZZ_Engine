/// @file    FluidRecipeLabels.hpp
/// @brief   TOML とリフレクションで共有する流体レシピの enum 名。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

namespace fbzz::asset::fluid_recipe_labels {

/// @note 添字はリフレクションで扱う enum 値、文字列は .fluid の保存名。
inline constexpr const char* KIND[] = { "gas", "liquid" };
inline constexpr const char* SHADING[] = { "smoke", "fire", "glow", "distortion", "liquid" };
inline constexpr const char* SOURCE_SHAPE[] = { "sphere", "box", "cone", "ring", "texture", "capsule", "cylinder" };
inline constexpr const char* FORCE_TYPE[] = { "wind", "attract", "repulse", "vortex", "noise", "drag" };
inline constexpr const char* COLLIDER_SHAPE[] = { "sphere", "box", "plane", "capsule", "cylinder" };
inline constexpr const char* BAKE_MODE[] = { "2d", "3d" };
inline constexpr const char* BAKE_SOLVER[] = { "auto", "gpu", "cpu" };

} // namespace fbzz::asset::fluid_recipe_labels
