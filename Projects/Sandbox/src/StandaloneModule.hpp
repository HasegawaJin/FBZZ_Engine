// FBZZ Engine
// StandaloneModule.hpp | fbzz::sandbox
// Sandbox のスタンドアロンゲーム実行 Module
#pragma once

#include <Engine/ProjectResolver.hpp>

#include <Engine/Core/IModule.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneManager.hpp>
#include <Physics/World.hpp>

#include <memory>

namespace fbzz::sandbox {

/// Application の共通ループから Standalone ゲーム更新・描画を駆動する Module。
/// WHY: Time / Input / Window / Memory / Profiler は Application に集約し、ゲーム固有処理だけをここに分離する。
class StandaloneModule : public core::IModule {
public:
    StandaloneModule(renderer::IRenderer& renderer,
                     renderer::ResourceManager& resources,
                     const LaunchProject& project,
                     const ProjectSettings& settings);

    [[nodiscard]] bool OnInit() override;
    void OnUpdate(float dt) override;
    void OnLateUpdate(float dt) override;
    void OnRender() override;
    void OnShutdown() override;

private:
    renderer::IRenderer&          m_renderer;
    renderer::ResourceManager&    m_resources;
    const LaunchProject&          m_project;
    const ProjectSettings&        m_settings;
    std::unique_ptr<scene::Scene> m_scene;
    physics::World                m_physicsWorld;
    scene::SceneManager           m_sceneManager;
};

} // namespace fbzz::sandbox
