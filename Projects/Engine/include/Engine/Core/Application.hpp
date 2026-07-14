// FBZZ Engine
// Application.hpp | fbzz::core
// エンジンのエントリポイントとメインループ
// Window / Renderer / Audio / SceneManager を一箇所で所有する Application シングルトン。
// sandbox や editor は Get() から各サブシステムへアクセスする。
#pragma once
#include "Engine/Core/Memory/MemorySystem.hpp"
#include "Engine/Core/Window.hpp"
#include "Engine/Renderer/IImGuiRenderer.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/RendererBackend.hpp"
#include "Engine/Scene/SceneManager.hpp"
#include <cstdint>
#include <memory>

namespace fbzz::audio { class AudioManager; class IAudioDevice; }

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
    // preferredBackend: プロジェクト設定で指定された描画バックエンド。ただしコマンドライン
    //   (--renderer=dx11 / dx12) が指定されていればそちらを優先する。
    bool Init(const Window::Config& windowConfig,
              renderer::RendererBackend preferredBackend = renderer::RendererBackend::DX12);
    void Shutdown();
    void Run();
    void Run(IModule& module);
    void Quit();

    bool                    IsRunning()      const { return m_isRunning; }
    Window&                 GetWindow()      const { return *m_window; }
    uint32_t                GetWindowWidth() const { return m_window ? m_window->GetWidth() : 0u; }
    uint32_t                GetWindowHeight() const { return m_window ? m_window->GetHeight() : 0u; }
    renderer::IRenderer&      GetRenderer()      const { return *m_renderer; }
    renderer::IImGuiRenderer& GetImGuiRenderer() const { return *m_imguiRenderer; }
    scene::SceneManager&      GetSceneManager() const { return *m_sceneManager; }
    // Audio初期化に失敗した場合はnullptrを返し、描画・編集機能だけで起動を継続する。
    audio::AudioManager*      GetAudioManager() const { return m_audioManager.get(); }
    MemorySystem&           GetMemorySystem() { return m_memorySystem; }
    const MemorySystem&     GetMemorySystem() const { return m_memorySystem; }

private:
    Application() = default;
    ~Application();

    bool m_isRunning = true;
    std::unique_ptr<Window>                   m_window;
    std::unique_ptr<renderer::IRenderer>      m_renderer;
    std::unique_ptr<renderer::IImGuiRenderer> m_imguiRenderer;
    std::unique_ptr<scene::SceneManager>      m_sceneManager;
    // WHY: AudioManagerはIAudioDeviceを非所有参照するため、Deviceを先に宣言して後から破棄する。
    std::unique_ptr<audio::IAudioDevice>      m_audioDevice;
    std::unique_ptr<audio::AudioManager>      m_audioManager;
    MemorySystem                         m_memorySystem;
};

} // namespace fbzz::core
