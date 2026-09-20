/// @file    ClothFormat.hpp
/// @brief   .cloth TOML のバージョンと CPU 布の入力上限。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <cstddef>

namespace fbzz::asset {
/// @note v3 は simulation_indices / render_bindings を追加。v1 / v2 も読み込む。
inline constexpr int CLOTH_FORMAT_VERSION = 3;
inline constexpr size_t CLOTH_MAX_VERTICES = 65536;
inline constexpr size_t CLOTH_MAX_INDICES = 393216;
inline constexpr size_t CLOTH_MAX_BONES = 128;
}
