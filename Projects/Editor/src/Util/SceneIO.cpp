/// @file    SceneIO.cpp
/// @brief   Editor wrapper for scene save/load and in-memory playmode snapshots.
/// @author  Hasegawa Jin
/// @date    2026-06-06
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/EditorSceneState.hpp>
#include <Editor/Util/EditorSerializer.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <cassert>
#include <vector>

namespace fbzz::editor {

namespace {

std::string s_snapshotDir  = "Assets/EditorConfig";
std::string s_snapshotPath = "Assets/EditorConfig/.playmode_snapshot.scene";
EditorSceneState* s_editorSceneState = nullptr;

} // namespace

void SceneIO::SetProjectRoot(const std::string& projectRoot)
{
    s_snapshotDir  = projectRoot + "/Assets/EditorConfig";
    s_snapshotPath = s_snapshotDir + "/.playmode_snapshot.scene";
}

void SceneIO::SetEditorSceneState(EditorSceneState* state)
{
    s_editorSceneState = state;
}

bool SceneIO::Save(const scene::Scene& scene, const std::string& path)
{
    // Engine の SceneSerializer::Save が非const Scene& を要求する設計になっているため const_cast で対応。
    // Save は概念的に読み取り専用 (シーンを変更しない) なので安全だが、
    // Engine 側が const 対応になったタイミングで除去すること。
    scene::Scene& mutableScene = const_cast<scene::Scene&>(scene);
    if (!scene::SceneSerializer::Save(mutableScene, path)) return false;
    if (s_editorSceneState) {
        std::vector<std::string> instanceIds;
        for (auto& go : mutableScene.GameObjects())
            if (!go.runtimeGenerated) instanceIds.push_back(go.instanceId);
        s_editorSceneState->PruneToInstances(instanceIds);
    }
    return !s_editorSceneState || EditorSerializer::Save(*s_editorSceneState, path);
}

bool SceneIO::Load(scene::Scene& scene, const std::string& path)
{
    (void)core::Application::Get().GetRenderer();
    auto* resources = renderer::ResourceManager::Active();
    assert(resources && "ResourceManager must be initialized before editor scene load");
    if (!scene::SceneSerializer::LoadInPlace(scene, path, *resources)) return false;
    return !s_editorSceneState || EditorSerializer::Load(*s_editorSceneState, path);
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
