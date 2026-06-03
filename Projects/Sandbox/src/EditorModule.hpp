// FBZZ Engine
// EditorModule.hpp | fbzz::sandbox
// Sandbox のエディタ実行 Module
#pragma once

#include <Engine/ProjectResolver.hpp>

#include <Editor/EditorApp.hpp>
#include <Engine/Core/IModule.hpp>
#include <Engine/Renderer/DebugCamera.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Vector3.hpp>
#include <Physics/Layer.hpp>
#include <Physics/World.hpp>

#include <memory>

namespace fbzz::sandbox {

/// Application の共通ループから Editor UI、PlayMode、Scene/Game viewport 描画を駆動する Module。
/// WHY: Editor 固有状態を main.cpp から切り離し、配布用 StandaloneModule と依存関係を分離しやすくする。
class EditorModule : public core::IModule {
public:
    EditorModule(renderer::IRenderer& renderer,
                 renderer::ResourceManager& resources,
                 const LaunchProject& project);

    [[nodiscard]] bool OnInit() override;
    void OnUpdate(float dt) override;
    void OnLateUpdate(float dt) override;
    void OnRender() override;
    void OnShutdown() override;

private:
    struct FocusAnim {
        bool          active   = false;
        math::Vector3 startPos = {};
        math::Vector3 endPos   = {};
        math::Vector3 target   = {};
        float         t        = 0.0f;
    };

    [[nodiscard]] float SimulationDeltaTime() const;
    void UpdateFocusAnimation(float dt);
    void RenderSceneViewport(renderer::ResourceHandle<renderer::RenderTargetTag> sceneRT);
    void RenderGameViewport(renderer::ResourceHandle<renderer::RenderTargetTag> gameRT,
                            const renderer::Camera& gameCamera,
                            fbzz::LayerMask gameCullingMask);
    void RenderEditorPanels();

    renderer::IRenderer&          m_renderer;
    renderer::ResourceManager&    m_resources;
    const LaunchProject&          m_project;
    editor::EditorApp             m_editorApp;
    std::unique_ptr<scene::Scene> m_scene;
    physics::World                m_physicsWorld;
    renderer::DebugCamera         m_debugCamera;
    FocusAnim                     m_focusAnim;
    float                         m_physicsAccumulator = 0.0f;
    float                         m_frameDt = 0.0f;
    bool                          m_stepFrame = false;
};

} // namespace fbzz::sandbox
