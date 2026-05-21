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
    (void)scene;
    (void)path;
    FBZZ_LOG_WARN("SceneSerializer::Load is currently unsupported in editor wrapper");
    return false;
}

std::string SceneSerializer::Serialize(const scene::Scene& scene)
{
    if (!Save(scene, kSnapshotPath)) return {};

    std::string text;
    if (!util::FileSystem::ReadText(kSnapshotPath, text)) {
        FBZZ_LOG_ERROR("SceneSerializer::Serialize read failed: %s", kSnapshotPath);
        return {};
    }
    return text;
}

bool SceneSerializer::Deserialize(scene::Scene& scene, const std::string& toml)
{
    (void)scene;
    (void)toml;
    FBZZ_LOG_WARN("SceneSerializer::Deserialize is currently unsupported in editor wrapper");
    return false;
}

} // namespace fbzz::editor
