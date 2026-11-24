/// @file    TomlFloatNormalization.hpp
/// @brief   保存用 TOML の浮動小数点正準化。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Engine/Api.hpp>

#include <toml++/impl/forward_declarations.hpp>

namespace fbzz::util {

/// @note 浮動小数点だけを再帰的に小数 6 桁へ丸め、負のゼロを除く。整数を含む他の値型は変更しない。
FBZZ_ENGINE_API void NormalizeTomlFloats(toml::node& node);

}
