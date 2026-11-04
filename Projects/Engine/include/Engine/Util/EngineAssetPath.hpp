/// @file    EngineAssetPath.hpp
/// @brief   SDK が提供する Engine アセットの探索と相対パス解決。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#pragma once
#include <Engine/Api.hpp>
#include <filesystem>

namespace fbzz::util {

/// @note SDK が提供する Engine アセットのルート (例: `<SDK>/share/fbzz/Assets`)。
/// @note 見つからない場合は空パスを返す。プロセス内で一度だけ解決する。
FBZZ_ENGINE_API const std::filesystem::path& EngineAssetRoot();

/// @note "Assets/..." 相対パスを実ファイルへ解決する。
/// @note カレントディレクトリとその祖先、最後に EngineAssetRoot() を順に探索し、
/// @note どこにも実体が無ければ requested をそのまま返す。
[[nodiscard]] FBZZ_ENGINE_API std::filesystem::path ResolveEngineAssetPath(const std::filesystem::path& requested);

} /// @note namespace fbzz::util
