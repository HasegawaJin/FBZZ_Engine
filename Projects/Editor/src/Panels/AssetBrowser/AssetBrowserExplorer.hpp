/// @file    AssetBrowserExplorer.hpp
/// @brief   AssetBrowser の選択を Explorer に渡す実体へ解決する。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once

#include <string>
#include <string_view>

namespace fbzz::editor {

struct AssetBrowserExplorerTarget {
    std::string path;
    bool isDirectory = false;
};

/// @return 解決不能・未存在なら path は空。成功時は UTF-8 の絶対パス。
/// @note 相対パスは projectRoot 基準。絶対パスと生成物の実体を保持し、仮想 Mesh / Sprite は元素材を示す。
[[nodiscard]] AssetBrowserExplorerTarget ResolveAssetBrowserExplorerTarget(
    std::string_view projectRoot, std::string_view assetPath);

/// @note Editor の UI スレッドで呼ぶ。ファイルは親で選択し、フォルダーはその中を開く。
/// @return 失敗はログと Toast で通知して false。
bool RevealAssetBrowserInExplorer(std::string_view projectRoot, std::string_view assetPath);

} /// @note namespace fbzz::editor
