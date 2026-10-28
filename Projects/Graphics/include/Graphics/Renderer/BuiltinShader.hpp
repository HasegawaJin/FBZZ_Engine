/// @file    BuiltinShader.hpp
/// @brief   外部アセットに依存しない診断シェーダーの識別子。
/// @author  Hasegawa Jin
/// @date    2026-10-05
#pragma once
#include <string_view>

namespace fbzz::renderer {

inline constexpr const char* ERROR_SURFACE_SHADER = "builtin:ErrorSurface";
inline constexpr const char* ERROR_SKINNED_SHADER = "builtin:ErrorSkinned";

[[nodiscard]] constexpr bool IsErrorShader(std::string_view path)
{
    return path == ERROR_SURFACE_SHADER || path == ERROR_SKINNED_SHADER;
}

} /// @note namespace fbzz::renderer
