/// @file    ProjectLauncher.cpp
/// @brief   プロジェクト起動シーケンスのユーティリティ実装。
/// @author  Hasegawa Jin
/// @date    2026-06-22
#include <Engine/Scene/ProjectLauncher.hpp>
#include <Engine/Scene/SceneManager.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Physics/World.hpp>
#include <filesystem>

namespace fbzz::scene {

void ProjectLauncher::ApplySettings(physics::World& world,
                                     SceneManager& sceneManager,
                                     const ProjectSettings& settings,
                                     UISystemContext* primaryUICtx)
{
    ApplyPhysicsSettings(world, settings);
    ApplyUISettings(settings, primaryUICtx);
    sceneManager.SetPhysicsHz(settings.physics.hz);
}

void ProjectLauncher::ApplyAdditionalUIContext(const ProjectSettings& settings,
                                                UISystemContext& ctx)
{
    ApplyUISettings(settings, &ctx);
}

void ProjectLauncher::RegisterScenesFromDirectory(SceneManager& sceneManager,
                                                    const std::filesystem::path& scenesDir,
                                                    renderer::ResourceManager& resources)
{
    std::error_code ec;
    if (!std::filesystem::exists(scenesDir, ec)) return;

    std::filesystem::recursive_directory_iterator it(
        scenesDir,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    const std::filesystem::recursive_directory_iterator end;
    while (it != end && !ec) {
        if (it->is_regular_file(ec) && it->path().extension() == L".scene") {
            sceneManager.RegisterFromFile(
                util::StringUtils::PathToUtf8(it->path().stem()),
                util::StringUtils::PathToUtf8(it->path()),
                resources);
        }
        it.increment(ec);
    }
}

} // namespace fbzz::scene
