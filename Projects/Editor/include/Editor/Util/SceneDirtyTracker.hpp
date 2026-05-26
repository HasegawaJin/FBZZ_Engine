// FBZZ Engine
// SceneDirtyTracker.hpp | fbzz::editor
// Tracks whether the active scene differs from the last clean snapshot
#pragma once
#include <cstddef>

namespace fbzz::scene { class Scene; }

namespace fbzz::editor {

class SceneDirtyTracker {
public:
    void CaptureClean(const scene::Scene& scene);
    bool Evaluate(const scene::Scene& scene);
    void MarkDirty();
    void Reset();

    bool IsDirty() const { return m_dirty; }

private:
    std::size_t m_cleanHash = 0;
    bool        m_hasCleanSnapshot = false;
    bool        m_dirty = false;
};

} // namespace fbzz::editor
