// FBZZ Engine
// Application.cpp | fbzz::core
// Application シングルトンの初期化とメインループ
// Window / Renderer / SceneManager を所有し、エンジン全体の寿命を管理する。
// sandbox 側で手動ループする場合も、初期化済みサブシステムの入口になる。
#define NOMINMAX
#include <Windows.h>
#include <timeapi.h>
#include "Engine/Core/Application.hpp"
#include "Engine/Core/IModule.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Input/Input.hpp"
#include "Engine/Profiler/ProfileScope.hpp"
#include "Engine/Profiler/Profiler.hpp"
#include "../Renderer/Platform/DX11/DX11ImGuiRenderer.hpp"
#include "../Renderer/Platform/DX11/DX11Renderer.hpp"

namespace fbzz::core {

Application& Application::Get() {
    static Application instance;
    return instance;
}

bool Application::Init() {
    // WHY: エディタは常にデフォルト設定 (1920x1080 ウィンドウ) で起動する。
    //      Standalone モードのみ ProjectSettings から取得した Config を渡す。
    return Init(Window::Config{});
}

bool Application::Init(const Window::Config& windowConfig) {
    timeBeginPeriod(1);

    // WHY: フレームアロケータとメモリ統計はエンジン全体の診断基盤なので、
    //      Window / Renderer より先に初期化し、以後のサブシステムから参照できる状態にする。
    constexpr std::size_t FRAME_ALLOCATOR_CAPACITY = 8u * 1024u * 1024u;
    if (!m_memorySystem.Initialize(FRAME_ALLOCATOR_CAPACITY))
        return false;

    m_window = std::make_unique<Window>();
    if (!m_window->Initialize(windowConfig))
        return false;

    input::Input::Init();

    auto dx11 = std::make_unique<renderer::DX11Renderer>();
    if (!dx11->Init(m_window->GetHandle(), m_window->GetWidth(), m_window->GetHeight()))
        return false;

    auto dx11ImGui = std::make_unique<renderer::DX11ImGuiRenderer>();
    if (!dx11ImGui->Init(dx11->GetDevice(), dx11->GetDeviceContext()))
        return false;

    m_renderer = std::move(dx11);
    m_imguiRenderer = std::move(dx11ImGui);

    m_window->SetResizeCallback([this](uint32_t w, uint32_t h) {
        m_renderer->Resize(w, h);
    });

    m_sceneManager = std::make_unique<scene::SceneManager>();

    FBZZ_LOG_INFO("Application started: %ux%u", m_window->GetWidth(), m_window->GetHeight());
    return true;
}

void Application::Shutdown() {
    m_imguiRenderer.reset();
    m_renderer.reset();
    if (m_window)
        m_window->Shutdown();
    m_memorySystem.Shutdown();
    FBZZ_LOG_INFO("Application shutdown (frames: %llu)", Time::frameCount);
    timeEndPeriod(1);
}

void Application::Run() {
    if (!Init()) return;

    while (m_isRunning) {
        Time::Tick();
        m_memorySystem.BeginFrame();
        profiler::Profiler::BeginFrame();

        {
            FBZZ_PROFILE_SCOPE("Input::Update");
            input::Input::Update();
        }

        {
            FBZZ_PROFILE_SCOPE("Window::PollEvents");
            m_window->PollEvents();
        }
        if (m_window->ShouldClose()) {
            profiler::Profiler::EndFrame();
            m_memorySystem.EndFrame();
            Quit();
            break;
        }

        {
            FBZZ_PROFILE_SCOPE("Renderer::Frame");
            m_renderer->BeginFrame();
            m_renderer->Clear({ 0.10f, 0.15f, 0.25f, 1.0f });
            m_renderer->EndFrame();
        }

        profiler::Profiler::EndFrame();
        m_memorySystem.EndFrame();
    }

    Shutdown();
}

void Application::Run(IModule& module) {
    // WHY: Init() やシーンロードにかかった時間を最初のゲームフレームの DeltaTime に混ぜない。
    //      先に Time::Tick() を呼んで時刻基準を作り、OnInit() 後の最初の Tick で実フレーム時間だけを得る。
    Time::Tick();

    if (!module.OnInit()) {
        module.OnShutdown();
        return;
    }

    while (m_isRunning) {
        Time::Tick();
        m_memorySystem.BeginFrame();
        profiler::Profiler::BeginFrame();

        {
            FBZZ_PROFILE_SCOPE("Input::Update");
            input::Input::Update();
        }

        {
            FBZZ_PROFILE_SCOPE("Window::PollEvents");
            m_window->PollEvents();
        }
        if (m_window->ShouldClose()) {
            // WHY: BeginFrame() 済みの Profiler / MemorySystem を必ず対で閉じる。
            //      break 前に EndFrame() することで終了フレームでも診断状態を壊さない。
            profiler::Profiler::EndFrame();
            m_memorySystem.EndFrame();
            Quit();
            break;
        }

        const float dt = Time::deltaTime;
        module.OnUpdate(dt);
        module.OnLateUpdate(dt);
        module.OnRender();

        profiler::Profiler::EndFrame();
        m_memorySystem.EndFrame();
    }

    module.OnShutdown();
}

void Application::Quit() {
    m_isRunning = false;
}

} // namespace fbzz::core
