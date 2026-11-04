/// @file    AssetCreation.hpp
/// @brief   UIと直接APIが共用するアセットの既定生成。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#pragma once
#include <string>
#include <string_view>
namespace fbzz::editor {
/// @note pathの上書きは禁止。presetは物理材質・Synthのプリセット名、DataAssetの型名。
[[nodiscard]] bool CreateEditorAsset(std::string_view kind, const std::string& path, const std::string& preset = {});
} /// @note namespace fbzz::editor
