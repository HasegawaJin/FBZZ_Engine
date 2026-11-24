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
    std::size_t m_cleanHash = 0;
    bool        m_hasCleanSnapshot = false;
    bool        m_dirty = false;
};

} /// @note namespace fbzz::editor
