/// @file    AssetBrowserFolders.hpp
/// @brief   Asset Browser と直接 API が共有するフォルダの範囲と色の形式。
/// @author  Hasegawa Jin
/// @date    2026-11-17
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::editor {

struct EditorContext;

struct AssetMount {
    std::string name;
    std::string path;
};

[[nodiscard]] std::vector<AssetMount> ResolveAssetBrowserMounts(
    const EditorContext& ctx, const std::string& assetsRoot);

/// @note ドット始まりの管理フォルダやインポート・コンパイルの生成先を除く。
[[nodiscard]] bool IsVisibleAssetBrowserFolder(const std::string& path);

/// @note Windows のパス比較と同様に ASCII の大文字・小文字と区切り文字の違いを吸収する。
[[nodiscard]] std::string AssetBrowserFolderKey(const std::string& path);

/// @return ImGui の packed 色を不透明な #RRGGBB として返す。
[[nodiscard]] std::string FormatAssetBrowserFolderColor(std::uint32_t color);

} /// @note namespace fbzz::editor
