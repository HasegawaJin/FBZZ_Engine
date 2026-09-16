/// @file    StandaloneApp.hpp
/// @brief   エディタ UI を持たないスタンドアロン (配布ゲーム) モードのライフサイクル管理。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// WHY: IModule を実装することで Application::Run() に乗せ、
/// Profiler / MemorySystem / Input::Update / PollEvents などの
/// フレーム境界処理をエンジン側に委譲する。
#pragma once
#include <Editor/ScriptDllLoader.hpp>
#include <Engine/Core/ILogSink.hpp>
#include <Engine/Core/IModule.hpp>
#include <Engine/ProjectResolver.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Scene/ProjectRuntime.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <filesystem>
#include <fstream>
#include <memory>

struct ImGuiContext;

namespace fbzz::renderer { class IImGuiRenderer; class IRenderer; }

namespace fbzz::editor_launcher {

class StandaloneApp final : public core::IModule {
public:
    // WHY settings が非 const か: `graphics` プロキシ (Option 画面) が実行中に
    //      settings.render を書き換えるため、その実体を可変で握る必要がある。
    //      配布ゲームでは ProjectSettings をファイルへ書き戻さない。
    StandaloneApp(renderer::IRenderer& renderer,
                  renderer::IImGuiRenderer& imguiRenderer,
                  renderer::ResourceManager& resources,
                  const LaunchProject& project,
                  ProjectSettings& settings);

    // IModule
    [[nodiscard]] bool OnInit()         override;
    void               OnUpdate(float dt)     override;
    void               OnLateUpdate(float dt) override;
    void               OnRender()       override;
    void               OnShutdown()     override;

private:
    // game.log へ書き出すシンク
    struct FileLogSink final : core::ILogSink {
        std::ofstream file;
        void OnLog(const core::LogEntry& entry) override;
    };

    renderer::IRenderer&             m_renderer;
    renderer::IImGuiRenderer&        m_imguiRenderer;
    renderer::ResourceManager&       m_resources;
    const LaunchProject&             m_project;
    ProjectSettings&                 m_settings;
    scene::ProjectRuntime            m_runtime;
    editor::ScriptDllLoader          m_scriptDll;
    FileLogSink                      m_logSink;
    ImGuiContext*                    m_imguiCtx     = nullptr;
    bool                             m_showProfiler = false;
};

} // namespace fbzz::editor_launcher
