/// @file    ManagedScriptMetadata.hpp
/// @brief   コンパイル済みの宣言情報と .cs.meta の型 GUID を候補 manifest へ結ぶ。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once
#include <filesystem>
#include <string>

namespace fbzz::editor {

/// @note missing Script の名前表示だけを復元する。factory 登録・GUID 生成・ファイル書き込みは行わない。
[[nodiscard]] bool LoadManagedScriptDisplayNames(const std::filesystem::path& projectRoot, std::string& diagnostic);

/// @note source checksum を検証してから個々の .meta を atomic replace する。複数ファイルの全体原子性は保証しない。
/// @note 削除された型の記録と未知の TOML section は保持し、partial の primary 宣言を推測しない。
[[nodiscard]] bool PrepareManagedScriptManifest(const std::filesystem::path& projectRoot,
    const std::filesystem::path& assembly, const std::string& discoveredSources, std::string& diagnostic);

} /// @note namespace fbzz::editor
