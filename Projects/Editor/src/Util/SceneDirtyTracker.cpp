/// @file    SceneDirtyTracker.cpp
/// @brief   Snapshot-based dirty state detection for editor scenes.
/// @author  Hasegawa Jin
/// @date    2026-05-26
#include <Editor/Util/SceneDirtyTracker.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <functional>
#include <optional>
#include <string>

namespace fbzz::editor {

namespace {

std::optional<std::size_t> HashScene(const scene::Scene& scene, const std::string& path)
{
    const std::string text = scene::SceneSerializer::CaptureSaveComparison(const_cast<scene::Scene&>(scene), path);
    if (text.empty()) return std::nullopt;
    return std::hash<std::string>{}(text);
}

} /// @note namespace

void SceneDirtyTracker::CaptureClean(const scene::Scene& scene, const std::string& path)
{
    const auto cleanHash = HashScene(scene, path);
    if (!cleanHash) {
        m_dirty = true;
        return;
    }
    CaptureCleanHash(*cleanHash);
}

void SceneDirtyTracker::CaptureCleanHash(std::size_t hash)
{
    m_cleanHash = hash;
    m_hasCleanSnapshot = true;
    m_dirty = false;
}

bool SceneDirtyTracker::Evaluate(const scene::Scene& scene, const std::string& path)
{
    if (!m_hasCleanSnapshot) {
        CaptureClean(scene, path);
        return m_dirty;
    }

    const auto currentHash = HashScene(scene, path);
    m_dirty = !currentHash || *currentHash != m_cleanHash;
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

} /// @note namespace fbzz::editor
