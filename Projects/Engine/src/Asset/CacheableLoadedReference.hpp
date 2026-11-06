/// @file    CacheableLoadedReference.hpp
/// @brief   読み込み済みアセットの索引付き参照だけを再利用対象にする。
/// @author  Hasegawa Jin
/// @date    2026-11-06
#pragma once
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>

namespace fbzz::asset::detail {
/// @note Call only after a successful load. Runtime Assets paths decoded from GUIDs retain indexed identity until revision/asset-generation changes.
/// @note Relative fallbacks, unindexed files and alternate spellings keep normal filesystem precedence; no negative result is retained.
/// @see Docs/design/instanced-mesh.md
inline bool IsCacheableLoadedReference(const std::string& reference)
{
    /// @note Startup-only polling mode supports filesystem-driven hosts without index notifications and same-binary profiling.
    static const bool polling = [] {
        const auto* mode = std::getenv("FBZZ_LOADED_REFERENCE_MODE");
        return mode && std::string_view(mode) == "poll";
    }();
    if (polling) return false;
    if (AssetDatabase::IsGuidRef(reference)) return true;
    if (!reference.starts_with("Assets/") || reference.find("/../") != std::string::npos
        || reference.find("/./") != std::string::npos || reference.find('\\') != std::string::npos) return false;
    const auto root = AssetDatabase::AssetsRoot();
    if (root.empty()) return false;
    const auto expected = root + reference.substr(7);
    const auto guid = AssetDatabase::TryGetGuidFromPath(expected);
    if (guid.empty()) return false;
    const auto same = [](const std::string& first, const std::string& second) {
        if (first.size() != second.size()) return false;
        for (size_t i = 0; i < first.size(); ++i) {
            const auto fold = [](unsigned char c) { return c == '\\' ? '/' : std::tolower(c); };
            if (fold(first[i]) != fold(second[i])) return false;
        }
        return true;
    };
    return same(AssetDatabase::PathFromGuid(guid), expected)
        && same(AssetManager::ResolveAssetPath(reference), expected);
}
} /// @note namespace fbzz::asset::detail
