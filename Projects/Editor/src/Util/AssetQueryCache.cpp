/// @file    AssetQueryCache.cpp
/// @brief   アセット検索の更新判定と結果所有をパネル間で共通化する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Editor/Util/AssetQueryCache.hpp>
#include <utility>

namespace fbzz::editor {

bool AssetQueryCache::Update(std::string_view query, std::span<const std::string> extensions, size_t limit)
{
    Key key{std::string(query), {extensions.begin(), extensions.end()}, limit, AssetSearch::Revision()};
    if (m_revision.IsCurrent(key)) return false;
    m_results.clear();
    const auto hits = AssetSearch::Query(key.query, key.extensions, key.limit);
    m_results.reserve(hits.size());
    for (const auto& hit : hits) m_results.push_back({*hit.entry, hit.score});
    m_revision.Commit(std::move(key));
    return true;
}

} /// @note namespace fbzz::editor
