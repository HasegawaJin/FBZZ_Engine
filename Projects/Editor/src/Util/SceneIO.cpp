/// @file    SceneIO.cpp
/// @brief   Editor wrapper for scene save/load and in-memory playmode snapshots.
/// @author  Hasegawa Jin
/// @date    2026-06-06
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/EditorSceneState.hpp>
#include <Editor/Util/EditorSerializer.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <utility>
#include <vector>

namespace fbzz::editor {

namespace {

std::string s_snapshotDir  = "Assets/EditorConfig";
std::string s_snapshotPath = "Assets/EditorConfig/.playmode_snapshot.scene";
EditorSceneState* s_editorSceneState = nullptr;

} /// @note namespace

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
    auto save = PrepareSave(const_cast<scene::Scene&>(scene), path, false);
    return save && save().succeeded;
}

std::function<SceneSaveResult()> SceneIO::PrepareSave(scene::Scene& scene,
    const std::string& path, bool backup, std::optional<std::filesystem::file_time_type> expectedWriteTime)
{
    std::string diagnostic;
    auto save = scene::SceneSerializer::PrepareSave(scene, backup ? std::string{} : path, diagnostic);
    if (!save.formatScene) return {};

    std::optional<EditorSceneState> metadata;
    if (s_editorSceneState) {
        metadata = *s_editorSceneState;
        if (!metadata->GetAllComponentOrders().empty() || !metadata->GetHiddenObjects().empty()
            || !metadata->GetLockedObjects().empty()) {
            std::vector<std::string> instanceIds;
            for (auto& go : scene.GameObjects())
                if (!go.runtimeGenerated) instanceIds.push_back(go.instanceId);
            metadata->PruneToInstances(instanceIds);
        }
    }

    return [path, save = std::move(save), metadata = std::move(metadata), expectedWriteTime, backup]() mutable {
        SceneSaveResult result;
        const auto diskPath = util::FileSystem::PathFromUtf8(path);
        const auto changed = [&] {
            return expectedWriteTime && util::FileSystem::LastWriteTime(diskPath) != *expectedWriteTime;
        };
        if (changed()) {
            result.diskChanged = true;
            return result;
        }
        const std::string text = save.formatScene();
        if (text.empty()) return result;
        if (changed()) {
            result.diskChanged = true;
            return result;
        }
        if (save.writeAssets && !save.writeAssets()) return result;
        if (changed()) {
            result.diskChanged = true;
            return result;
        }
        if (!util::FileSystem::WriteTextAtomic(path, text)) return result;
        result.writeTime = util::FileSystem::LastWriteTime(diskPath);
        result.succeeded = !metadata || EditorSerializer::Save(*metadata, path);
        if (result.succeeded && !backup) result.contentHash = std::hash<std::string>{}(text);
        return result;
    };
}

bool SceneIO::Load(scene::Scene& scene, const std::string& path)
{
    auto prepared = scene::SceneSerializer::Load(path, scene.GetResourceManager());
    if (!prepared) return false;
    scene = std::move(*prepared);
    return !s_editorSceneState || EditorSerializer::Load(*s_editorSceneState, path);
}

std::string SceneIO::Serialize(const scene::Scene& scene)
{
    /// @note Engine の SceneSerializer::Save が非 const Scene& を要求する設計になっているため
    /// @note const_cast で対応。直列化はシーンを変更しない。
    scene::Scene& mutableScene = const_cast<scene::Scene&>(scene);

    /// @note ディスクを経由しない。書き込み先が無いだけで空文字を返すと、呼び出し側 (変更検知・
    /// @note プレハブ切り出し) には «中身が空 = 変更なし» に見えてしまう。
    std::string text = scene::SceneSerializer::SaveToText(mutableScene);
    if (text.empty())
        FBZZ_LOG_ERROR("SceneIO::Serialize produced an empty snapshot");
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
    return scene::SceneSerializer::AppendObjects(scene, toml, scene.GetResourceManager(), outRoots);
}

} /// @note namespace fbzz::editor
