// FBZZ Engine
// SceneSerializer.cpp | fbzz::editor
// Editor wrapper for scene save/load and in-memory playmode snapshots
#include <editor/Util/SceneSerializer.hpp>
#include <engine/Core/Application.hpp>
#include <engine/Core/Logger.hpp>
#include <engine/Scene/SceneSerializer.hpp>
#include <engine/Util/FileSystem.hpp>

namespace fbzz::editor {

namespace {

constexpr const char* kSnapshotPath = "editor_config/.playmode_snapshot.fbzz";

} // namespace

bool SceneSerializer::Save(const scene::Scene& scene, const std::string& path)
{
    // engine serializer currently expects non-const Scene&
    scene::Scene& mutableScene = const_cast<scene::Scene&>(scene);
    return scene::SceneSerializer::Save(mutableScene, path);
}

bool SceneSerializer::Load(scene::Scene& scene, const std::string& path)
{
    auto& renderer = core::Application::Get().GetRenderer();
    return scene::SceneSerializer::LoadInPlace(scene, path, renderer);
}

std::string SceneSerializer::Serialize(const scene::Scene& scene)
{
    if (!Save(scene, kSnapshotPath)) {
        FBZZ_LOG_ERROR("SceneSerializer::Serialize save failed: %s", kSnapshotPath);
        return {};
    }

    std::string text;
    if (!util::FileSystem::ReadText(kSnapshotPath, text)) {
        FBZZ_LOG_ERROR("SceneSerializer::Serialize read failed: %s", kSnapshotPath);
        return {};
    }
    if (text.empty()) {
        FBZZ_LOG_ERROR("SceneSerializer::Serialize produced an empty snapshot");
        return {};
    }
    return text;
}

bool SceneSerializer::Deserialize(scene::Scene& scene, const std::string& toml)
{
    if (toml.empty()) {
        FBZZ_LOG_ERROR("SceneSerializer::Deserialize rejected an empty snapshot");
        return false;
    }
    if (!util::FileSystem::WriteText(kSnapshotPath, toml)) {
        FBZZ_LOG_ERROR("SceneSerializer::Deserialize write failed: %s", kSnapshotPath);
        return false;
    }
    return Load(scene, kSnapshotPath);
}

} // namespace fbzz::editor
