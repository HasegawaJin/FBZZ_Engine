// FBZZ Engine
// StandaloneApp.hpp | fbzz::editor_launcher
// エディタ UI を持たないスタンドアロン (配布ゲーム) モードのライフサイクル管理。
//
// WHY: IModule を実装することで Application::Run() に乗せ、
//      Profiler / MemorySystem / Input::Update / PollEvents などの
//      フレーム境界処理をエンジン側に委譲する。
#pragma once
#include <Editor/ScriptDllLoader.hpp>
#include <Engine/Core/ILogSink.hpp>
#include <Engine/Core/IModule.hpp>
#include <Engine/ProjectResolver.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Physics/World.hpp>
#include <filesystem>
#include <fstream>
#include <memory>

struct ImGuiContext;

namespace fbzz::renderer { class IRenderer; }

namespace fbzz::editor_launcher {

class StandaloneApp final : public core::IModule {
public:
    StandaloneApp(renderer::IRenderer& renderer,
                  renderer::ResourceManager& resources,
                  const LaunchProject& project,
                  const ProjectSettings& settings);

    // IModule
    [[nodiscard]] bool OnInit()         override;
    void               OnUpdate(float dt)     override;
    void               OnLateUpdate(float dt) override;
    void               OnRender()       override;
    void               OnShutdown()     override;

private:
    renderer::Camera ResolveGameCamera(float aspectRatio) const;

    // game.log へ書き出すシンク
    struct FileLogSink final : core::ILogSink {
        std::ofstream file;
        void OnLog(const core::LogEntry& entry) override;
    };

    renderer::IRenderer&             m_renderer;
    renderer::ResourceManager&       m_resources;
    const LaunchProject&             m_project;
    const ProjectSettings&           m_settings;
    std::unique_ptr<scene::Scene>    m_scene;
    std::unique_ptr<physics::World>  m_physicsWorld;
    editor::ScriptDllLoader          m_scriptDll;
    FileLogSink                      m_logSink;
    ImGuiContext*                    m_imguiCtx          = nullptr;
    bool                             m_showProfiler      = false;
    float                            m_physicsAccumulator = 0.0f;
};

} // namespace fbzz::editor_launcher
