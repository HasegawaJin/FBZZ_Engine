/// @file    ManagedDataAssets.cpp
/// @brief   native アセット解決と managed DataAsset の純値読取。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include "ManagedDataAssets.hpp"
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <filesystem>
#include <sstream>
#include <toml++/toml.hpp>

namespace fbzz::scene::managed_detail {
ManagedBridgeStatus ReadDataAsset(std::string_view reference, std::string& json)
{
    if (reference.empty() || reference.size() > 4096 || reference.find('\0') != std::string_view::npos)
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    const auto resolved = asset::AssetManager::ResolveAssetPath(std::string(reference));
    const auto resolvedPath = util::FileSystem::PathFromUtf8(resolved);
    if (resolved.empty() || resolvedPath.extension() != ".fzdata") return ManagedBridgeStatus::INVALID_ARGUMENT;
    std::error_code error;
    (void)std::filesystem::file_size(resolvedPath, error);
    if (error) return ManagedBridgeStatus::NOT_FOUND;
    std::string text;
    if (!util::FileSystem::ReadText(resolved, text)) return ManagedBridgeStatus::NOT_FOUND;
    auto parsed = toml::parse(text);
    if (!parsed || parsed.table()["type"].value_or(std::string{}).empty()) return ManagedBridgeStatus::LOAD_FAILED;
    /// @note Portable managed documents keep their canonical typed value JSON in TOML; parsing occurs once per owned runtime read.
    if (parsed.table()["formatVersion"].value_or(0) == 2) {
        const auto value = parsed.table()["json"].value<std::string>();
        if (!value) return ManagedBridgeStatus::LOAD_FAILED;
        json = *value; return ManagedBridgeStatus::OK;
    }
    std::ostringstream output;
    output << toml::json_formatter(parsed.table());
    json = output.str();
    return ManagedBridgeStatus::OK;
}
} /// @note namespace fbzz::scene::managed_detail
