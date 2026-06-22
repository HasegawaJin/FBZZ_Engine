// FBZZ Engine
// StandaloneProjectModule.hpp | fbzz::scene
// Standaloneプロジェクト共通のゲーム更新・描画モジュール
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

/// SandboxとGameHub生成プロジェクトで共通のStandalone実行ループを提供する。
/// WHY: 各プロジェクトが初期化・更新・描画・終了処理を複製すると、片方で修正した
///      ランタイム不具合がもう片方に残るため、実行経路全体をEngine側に集約する。
class StandaloneProjectModule final : public core::IModule {
public:
    /// プロジェクトのAssetsルートと開始シーンを使ってStandalone実行を構成する。
    StandaloneProjectModule(renderer::IRenderer& renderer,
                            renderer::ResourceManager& resources,
                            std::filesystem::path projectRoot,
                            std::filesystem::path startSceneFile,
                            const ProjectSettings& settings);

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
    const ProjectSettings&     m_settings;
    ProjectRuntime             m_runtime;
};

} // namespace fbzz::scene
