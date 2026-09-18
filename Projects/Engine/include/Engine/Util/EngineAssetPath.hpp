/// @file    EngineAssetPath.hpp
/// @brief   SDK が提供する Engine アセットの探索と相対パス解決。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#pragma once
#include <filesystem>

namespace fbzz::util {

/// SDK が提供する Engine アセットのルート (例: `<SDK>/share/fbzz/Assets`)。
/// 見つからない場合は空パスを返す。プロセス内で一度だけ解決する。
const std::filesystem::path& EngineAssetRoot();

/// "Assets/..." 相対パスを実ファイルへ解決する。
/// カレントディレクトリとその祖先、最後に EngineAssetRoot() を順に探索し、
/// どこにも実体が無ければ requested をそのまま返す。
[[nodiscard]] std::filesystem::path ResolveEngineAssetPath(const std::filesystem::path& requested);

} // namespace fbzz::util
