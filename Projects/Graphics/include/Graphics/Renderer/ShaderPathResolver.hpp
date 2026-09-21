/// @file    ShaderPathResolver.hpp
/// @brief   ホストが設定するシェーダーファイルの探索境界。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <filesystem>

namespace fbzz::renderer {
using ShaderPathResolver = std::filesystem::path (*)(const std::filesystem::path&);
/// @note 描画開始前に設定し、ホスト DLL の破棄前に nullptr へ戻す。未設定なら指定パスをそのまま使う。
void SetShaderPathResolver(ShaderPathResolver resolver);
[[nodiscard]] std::filesystem::path ResolveShaderFilePath(const std::filesystem::path& requested);
}
