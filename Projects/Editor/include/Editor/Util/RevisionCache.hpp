/// @file    RevisionCache.hpp
/// @brief   成功済み処理の入力キーを保持し、未変更フレームの再処理を避ける。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <optional>
#include <utility>

namespace fbzz::editor {

/// @note Key は内容と寿命の世代を含む値型。処理成功後だけ Commit し、失敗をキャッシュしない。
template<typename Key>
class RevisionCache {
public:
    [[nodiscard]] bool IsCurrent(const Key& key) const { return m_key && *m_key == key; }
    void Commit(Key key) { m_key = std::move(key); }
    void Invalidate() { m_key.reset(); }
private:
    std::optional<Key> m_key;
};

} /// @note namespace fbzz::editor
