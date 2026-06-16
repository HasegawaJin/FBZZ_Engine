// FBZZ Engine
// SceneIO.cpp | fbzz::editor
// Editor wrapper for scene save/load and in-memory playmode snapshots
#include <Editor/Util/SceneIO.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <cassert>

namespace fbzz::editor {

namespace {

std::string s_snapshotDir  = "Assets/EditorConfig";
std::string s_snapshotPath = "Assets/EditorConfig/.playmode_snapshot.scene";

} // namespace

void SceneIO::SetProjectRoot(const std::string& projectRoot)
{
    s_snapshotDir  = projectRoot + "/Assets/EditorConfig";
    s_snapshotPath = s_snapshotDir + "/.playmode_snapshot.scene";
}

bool SceneIO::Save(const scene::Scene& scene, const std::string& path)
{
    // Engine の SceneSerializer::Save が非const Scene& を要求する設計になっているため const_cast で対応。
    // Save は概念的に読み取り専用 (シーンを変更しない) なので安全だが、
    // Engine 側が const 対応になったタイミングで除去すること。
    scene::Scene& mutableScene = const_cast<scene::Scene&>(scene);
    return scene::SceneSerializer::Save(mutableScene, path);
}

bool SceneIO::Load(scene::Scene& scene, const std::string& path)
{
    (void)core::Application::Get().GetRenderer();
    auto* resources = renderer::ResourceManager::Active();
    assert(resources && "ResourceManager must be initialized before editor scene load");
    return scene::SceneSerializer::LoadInPlace(scene, path, *resources);
}

std::string SceneIO::Serialize(const scene::Scene& scene)
{
    // メモリ上の TOML 文字列を返したいが、Engine 側 API がファイル経由のみ対応しているため
    // 一時ファイル (s_snapshotPath) を介してテキストを読み戻す。
    (void)util::FileSystem::EnsureDirectory(s_snapshotDir);
    if (!Save(scene, s_snapshotPath)) {
        FBZZ_LOG_ERROR("SceneIO::Serialize save failed: %s", s_snapshotPath.c_str());
        return {};
    }

    std::string text;
    if (!util::FileSystem::ReadText(s_snapshotPath, text)) {
        FBZZ_LOG_ERROR("SceneIO::Serialize read failed: %s", s_snapshotPath.c_str());
        return {};
    }
    if (text.empty()) {
        FBZZ_LOG_ERROR("SceneIO::Serialize produced an empty snapshot");
        return {};
    }
    return text;
}

bool SceneIO::Deserialize(scene::Scene& scene, const std::string& toml)
{
    if (toml.empty()) {
        FBZZ_LOG_ERROR("SceneIO::Deserialize rejected an empty snapshot");
        return false;
    }
    (void)util::FileSystem::EnsureDirectory(s_snapshotDir);
    if (!util::FileSystem::WriteText(s_snapshotPath, toml)) {
        FBZZ_LOG_ERROR("SceneIO::Deserialize write failed: %s", s_snapshotPath.c_str());
        return false;
    }
    return Load(scene, s_snapshotPath);
}

bool SceneIO::AppendObjects(scene::Scene& scene, const std::string& toml,
                             std::vector<scene::EntityID>& outRoots)
{
    if (toml.empty()) return false;
    auto* resources = renderer::ResourceManager::Active();
    assert(resources && "ResourceManager must be initialized");
    return scene::SceneSerializer::AppendObjects(scene, toml, *resources, outRoots);
}

} // namespace fbzz::editor
