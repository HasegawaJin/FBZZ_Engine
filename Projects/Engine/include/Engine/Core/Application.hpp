// FBZZ Engine
// Application.hpp | fbzz::core
// エンジンのエントリポイントとメインループ
// Window / Renderer / SceneManager を一箇所で所有する Application シングルトン。
// sandbox や editor は Get() から各サブシステムへアクセスする。
#pragma once
#include "Engine/Core/Memory/MemorySystem.hpp"
#include "Engine/Core/Window.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Scene/SceneManager.hpp"
#include <memory>

namespace fbzz::core {

class IModule;

class Application {
public:
    static Application& Get();

    // デフォルト設定 (エディタモード) で初期化する。
    bool Init();
    // Standalone モード用: ウィンドウ設定を外部から指定して初期化する。
    // ProjectSettings を Application::Init() より前に読み込んでウィンドウを正しいサイズで生成するため、
    // Window::Config を受け取るオーバーロードを別途用意する。
    bool Init(const Window::Config& windowConfig);
    void Shutdown();
    void Run();
    void Run(IModule& module);
    void Quit();

    bool                    IsRunning()      const { return m_isRunning; }
    Window&                 GetWindow()      const { return *m_window; }
    renderer::IRenderer&    GetRenderer()    const { return *m_renderer; }
    scene::SceneManager&    GetSceneManager() const { return *m_sceneManager; }
    MemorySystem&           GetMemorySystem() { return m_memorySystem; }
    const MemorySystem&     GetMemorySystem() const { return m_memorySystem; }

private:
    Application() = default;

    bool m_isRunning = true;
    std::unique_ptr<Window>              m_window;
    std::unique_ptr<renderer::IRenderer> m_renderer;
    std::unique_ptr<scene::SceneManager> m_sceneManager;
    MemorySystem                         m_memorySystem;
};

} // namespace fbzz::core
