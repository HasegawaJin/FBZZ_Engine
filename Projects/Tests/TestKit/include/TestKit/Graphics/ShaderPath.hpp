/// @file    ShaderPath.hpp
/// @brief   GPU テストで本番シェーダーの相対パスをソースルートへ解決する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once

#include <filesystem>
#include <string_view>

namespace fbzz::testkit {

/// @note Assets/Shaders/ で始まる指定だけを shaderRoot へ移す。絶対パスや別の相対パスは実在確認せずそのまま返す。
[[nodiscard]] inline std::filesystem::path ResolveShaderPath(
    const std::filesystem::path& requested, const std::filesystem::path& shaderRoot)
{
    const auto relative = requested.generic_string();
    constexpr std::string_view SHADER_PREFIX = "Assets/Shaders/";
    if (relative.starts_with(SHADER_PREFIX))
        return shaderRoot / relative.substr(SHADER_PREFIX.size());
    return requested;
}

}
