/// @file    StandaloneProjectModule.hpp
/// @brief   Standaloneプロジェクト共通のゲーム更新・描画モジュール。
/// @author  Hasegawa Jin
/// @date    2026-06-22
#pragma once

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
    StandaloneProjectModule(renderer::IRenderer& renderer,
                            renderer::ResourceManager& resources,
                            std::filesystem::path projectRoot,
                            std::filesystem::path startSceneFile,
                            ProjectSettings& settings);

    [[nodiscard]] bool OnInit() override;
    void OnUpdate(float dt) override;
    void OnLateUpdate(float dt) override;
    void OnRender() override;
    void OnShutdown() override;

private:
    renderer::IRenderer&       m_renderer;
    renderer::ResourceManager& m_resources;
    std::filesystem::path      m_projectRoot;
    std::filesystem::path      m_startSceneFile;
    ProjectSettings&           m_settings;
    ProjectRuntime             m_runtime;
};

} // namespace fbzz::scene
