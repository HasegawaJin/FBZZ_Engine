/// @file    AssetQueryCache.hpp
/// @brief   索引世代・検索条件が変わったときだけ所有済みアセット検索結果を更新する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Editor/Util/AssetSearch.hpp>
#include <Editor/Util/RevisionCache.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fbzz::editor {

struct CachedAssetHit {
    AssetSearchEntry entry;
    int score = 0;
};

/// @note UI が保持する結果は値で所有し、索引の再配置・再構築後も dangling pointer にしない。
class AssetQueryCache {
public:
    bool Update(std::string_view query, std::span<const std::string> extensions, size_t limit);
    [[nodiscard]] const std::vector<CachedAssetHit>& Results() const { return m_results; }
    void Invalidate() { m_revision.Invalidate(); }
private:
    struct Key {
        std::string query;
        std::vector<std::string> extensions;
        size_t limit = 0;
        uint64_t indexRevision = 0;
        bool operator==(const Key&) const = default;
    };
    RevisionCache<Key> m_revision;
    std::vector<CachedAssetHit> m_results;
};

} /// @note namespace fbzz::editor
