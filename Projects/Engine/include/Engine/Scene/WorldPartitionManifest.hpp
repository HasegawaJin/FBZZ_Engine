/// @file    WorldPartitionManifest.hpp
/// @brief   Validated, versioned world-cell payload and dependency persistence.
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
    /// @note Normalized project-relative Assets/... paths, resolved against the owning project root.
    std::string nearPath;
    /// @note Empty means this cell has no distant representation.
    std::string hlodPath;
    std::vector<WorldPartitionDependency> dependencies;
};

struct WorldPartitionManifest {
    WorldCellGrid grid;
    uint64_t revision = 1;
    std::vector<WorldPartitionCell> cells;
    /// @note Authored source forest identities retired only in the Play clone before resident cells publish.
    std::vector<std::string> sourceInstanceIds;
};

/// @note Geometry bounds may extend beyond the owning cell; XZ ownership is determined by grid and id.
[[nodiscard]] FBZZ_ENGINE_API bool ValidateWorldPartitionManifest(
    const WorldPartitionManifest& manifest, std::string& diagnostic);
/// @return Failure leaves out unchanged. uint64 revisions are preserved as decimal TOML strings.
[[nodiscard]] FBZZ_ENGINE_API bool SerializeWorldPartitionManifest(
    const WorldPartitionManifest& manifest, std::string& out, std::string& diagnostic);
/// @return Unsupported versions, malformed paths, duplicate cells or invalid ranges leave out unchanged.
[[nodiscard]] FBZZ_ENGINE_API bool DeserializeWorldPartitionManifest(
    std::string_view text, WorldPartitionManifest& out, std::string& diagnostic);
[[nodiscard]] FBZZ_ENGINE_API bool LoadWorldPartitionManifest(
    std::string_view path, WorldPartitionManifest& out, std::string& diagnostic);
/// @note Atomic replacement preserves the previous saved manifest on validation or write failure.
[[nodiscard]] FBZZ_ENGINE_API bool SaveWorldPartitionManifest(
    std::string_view path, const WorldPartitionManifest& manifest, std::string& diagnostic);

} /// @note namespace fbzz::scene
