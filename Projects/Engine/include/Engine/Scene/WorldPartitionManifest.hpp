/// @file    WorldPartitionManifest.hpp
/// @brief   検証とバージョン管理を伴うワールドセルの内容と依存関係の保存。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/WorldCell.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {

struct WorldPartitionDependency {
    std::string typeName;
    std::string reference;
    bool operator==(const WorldPartitionDependency&) const = default;
};

struct WorldPartitionCell {
    WorldCellId id;
    WorldCellBounds bounds;
    uint64_t revision = 1;
    /// @note プロジェクトルートから解決する、正規化済みの Assets/... 相対パス。
    std::string nearPath;
    /// @note 空の場合、このセルには遠距離表現がない。
    std::string hlodPath;
    std::vector<WorldPartitionDependency> dependencies;
};

struct WorldPartitionManifest {
    WorldCellGrid grid;
    uint64_t revision = 1;
    std::vector<WorldPartitionCell> cells;
    /// @note 制作時の元階層の ID。常駐セルの公開前に退役するのは Play の複製内だけ。
    std::vector<std::string> sourceInstanceIds;
};

/// @note 形状の境界は所属セルの外まで広がり得る。XZ の所属は格子と ID で決まる。
[[nodiscard]] FBZZ_ENGINE_API bool ValidateWorldPartitionManifest(
    const WorldPartitionManifest& manifest, std::string& diagnostic);
/// @return 失敗時は out を変更しない。uint64 の版は十進数の TOML 文字列として保持する。
[[nodiscard]] FBZZ_ENGINE_API bool SerializeWorldPartitionManifest(
    const WorldPartitionManifest& manifest, std::string& out, std::string& diagnostic);
/// @return 未対応の版、不正なパス、重複セル、不正範囲の場合は out を変更しない。
[[nodiscard]] FBZZ_ENGINE_API bool DeserializeWorldPartitionManifest(
    std::string_view text, WorldPartitionManifest& out, std::string& diagnostic);
[[nodiscard]] FBZZ_ENGINE_API bool LoadWorldPartitionManifest(
    std::string_view path, WorldPartitionManifest& out, std::string& diagnostic);
/// @note アトミックな置き換えにより、検証や書き込みに失敗しても保存済みのマニフェストが残る。
[[nodiscard]] FBZZ_ENGINE_API bool SaveWorldPartitionManifest(
    std::string_view path, const WorldPartitionManifest& manifest, std::string& diagnostic);

}
