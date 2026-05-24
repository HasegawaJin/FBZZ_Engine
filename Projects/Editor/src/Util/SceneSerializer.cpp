// FBZZ Engine
// SceneSerializer.cpp | fbzz::editor
// Editor wrapper for scene save/load and in-memory playmode snapshots
#include <Editor/Util/SceneSerializer.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <cassert>

namespace fbzz::editor {

namespace {

constexpr const char* kSnapshotPath = "editor_config/.playmode_snapshot.fbzz";

} // namespace

bool SceneSerializer::Save(const scene::Scene& scene, const std::string& path)
{
    // Engine の SceneSerializer::Save が非const Scene& を要求する設計になっているため const_cast で対応。
    // Save は概念的に読み取り専用 (シーンを変更しない) なので安全だが、
    // Engine 側が const 対応になったタイミングで除去すること。
    scene::Scene& mutableScene = const_cast<scene::Scene&>(scene);
    return scene::SceneSerializer::Save(mutableScene, path);
}

bool SceneSerializer::Load(scene::Scene& scene, const std::string& path)
{
    (void)core::Application::Get().GetRenderer();
    auto* resources = renderer::ResourceManager::Active();
    assert(resources && "ResourceManager must be initialized before editor scene load");
    return scene::SceneSerializer::LoadInPlace(scene, path, *resources);
}

std::string SceneSerializer::Serialize(const scene::Scene& scene)
{
    // メモリ上の TOML 文字列を返したいが、Engine 側 API がファイル経由のみ対応しているため
    // 一時ファイル (kSnapshotPath) を介してテキストを読み戻す。
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
