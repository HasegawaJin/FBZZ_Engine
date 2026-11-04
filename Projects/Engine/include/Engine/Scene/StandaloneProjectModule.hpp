/// @file    StandaloneProjectModule.hpp
/// @brief   Standaloneプロジェクト共通のゲーム更新・描画モジュール。
/// @author  Hasegawa Jin
/// @date    2026-06-22
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Core/IModule.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Scene/ProjectRuntime.hpp>

#include <filesystem>

namespace fbzz::renderer {
class IRenderer;
class ResourceManager;
}

namespace fbzz::scene {

/// @brief Sandbox と GameHub 生成プロジェクトで共通の Standalone 実行ループを提供する。
/// @note 各プロジェクトが初期化・更新・描画・終了処理を複製すると、片方で修正したランタイム不具合がもう片方に残るため、実行経路全体を Engine 側に集約する。
class StandaloneProjectModule final : public core::IModule {
public:
    /// @brief プロジェクトの Assets ルートと開始シーンを使って Standalone 実行を構成する。
    /// @param settings 非 const。`graphics` プロキシ (Option 画面) が実行中に `settings.render` を書き換えるため可変で握る。配布ゲームは ProjectSettings をファイルへ書き戻さないので、書き換えはプロセスが生きている間だけ効く。
    FBZZ_ENGINE_API StandaloneProjectModule(renderer::IRenderer& renderer,
                            renderer::ResourceManager& resources,
                            std::filesystem::path projectRoot,
                            std::filesystem::path startSceneFile,
                            ProjectSettings& settings);

    /// @note Editor Play can select an exact scene file without changing ProjectSettings or resolving a duplicate stem.
    FBZZ_ENGINE_API StandaloneProjectModule(renderer::IRenderer& renderer,
                            renderer::ResourceManager& resources,
                            std::filesystem::path projectRoot,
                            std::filesystem::path startSceneFile,
                            ProjectSettings& settings,
                            bool exactStartScene);

    [[nodiscard]] FBZZ_ENGINE_API bool OnInit() override;
    FBZZ_ENGINE_API void OnUpdate(float dt) override;
    FBZZ_ENGINE_API void OnLateUpdate(float dt) override;
    FBZZ_ENGINE_API void OnRender() override;
    FBZZ_ENGINE_API void OnShutdown() override;
    /// @note Hosts use this after Application::Run to distinguish startup failure from a normal game exit.
    [[nodiscard]] bool StartedSuccessfully() const { return m_startedSuccessfully; }

private:
    renderer::IRenderer&       m_renderer;
    renderer::ResourceManager& m_resources;
    std::filesystem::path      m_projectRoot;
    std::filesystem::path      m_startSceneFile;
    ProjectSettings&           m_settings;
    ProjectRuntime             m_runtime;
    bool                       m_managedLoaded = false;
    bool                       m_exactStartScene = false;
    bool                       m_startedSuccessfully = false;
    bool                       m_waitingForStartScene = false;
};

} /// @note namespace fbzz::scene
