// FBZZ Engine
// SceneDirtyTracker.cpp | fbzz::editor
// Snapshot-based dirty state detection for editor scenes
#include <Editor/Util/SceneDirtyTracker.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Engine/Scene/Scene.hpp>
#include <functional>
#include <string>

namespace fbzz::editor {

namespace {

std::size_t HashScene(const scene::Scene& scene)
{
    const std::string text = SceneIO::Serialize(scene);
    if (text.empty()) return 0;
    return std::hash<std::string>{}(text);
}

} // namespace

void SceneDirtyTracker::CaptureClean(const scene::Scene& scene)
{
    m_cleanHash = HashScene(scene);
    m_hasCleanSnapshot = m_cleanHash != 0;
    m_dirty = false;
}

bool SceneDirtyTracker::Evaluate(const scene::Scene& scene)
{
    if (!m_hasCleanSnapshot) {
        CaptureClean(scene);
        return m_dirty;
    }

    const std::size_t currentHash = HashScene(scene);
    if (currentHash != 0)
        m_dirty = currentHash != m_cleanHash;
    return m_dirty;
}

void SceneDirtyTracker::MarkDirty()
{
    m_dirty = true;
}

void SceneDirtyTracker::Reset()
{
    m_cleanHash = 0;
    m_hasCleanSnapshot = false;
    m_dirty = false;
}

} // namespace fbzz::editor
