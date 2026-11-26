/// @file    SceneDirtyTracker.hpp
/// @brief   Tracks whether the active scene differs from the last clean snapshot.
/// @author  Hasegawa Jin
/// @date    2026-05-26
#pragma once
#include <cstddef>
#include <string>

namespace fbzz::scene { class Scene; }

namespace fbzz::editor {

class SceneDirtyTracker {
public:
    void CaptureClean(const scene::Scene& scene, const std::string& path = {});
    void CaptureCleanHash(std::size_t hash);
    bool Evaluate(const scene::Scene& scene, const std::string& path = {});
    void MarkDirty();
    void Reset();

    bool IsDirty() const { return m_dirty; }

private:
    std::size_t m_cleanHash = 0; ///< 最後に保存済みとして記録したシーン内容のハッシュ。
    bool        m_hasCleanSnapshot = false; ///< 現在のシーンと比較できる保存済みの基準があるか。
    bool        m_dirty = false; ///< 最後の評価または明示的な変更通知で、未保存と判定したか。
};

} // namespace fbzz::editor
