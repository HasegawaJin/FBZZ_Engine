// FBZZ Engine
// Application.cpp | fbzz::core
// エンジンのエントリポイントとメインループ
#include "Engine/Core/Application.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Input/Input.hpp"
#include "../Renderer/Platform/DX11/DX11Renderer.hpp"

namespace fbzz::core {

Application& Application::Get() {
    static Application instance;
    return instance;
}

bool Application::Init() {
    Window::Config windowConfig;
    m_window = std::make_unique<Window>();
    if (!m_window->Initialize(windowConfig))
        return false;

    input::Input::Init();

    auto dx11 = std::make_unique<renderer::DX11Renderer>();
    if (!dx11->Init(m_window->GetHandle(), m_window->GetWidth(), m_window->GetHeight()))
        return false;
    m_renderer = std::move(dx11);

    m_window->SetResizeCallback([this](uint32_t w, uint32_t h) {
        m_renderer->Resize(w, h);
    });

    m_sceneManager = std::make_unique<scene::SceneManager>();

    FBZZ_LOG_INFO("Application 起動: %ux%u", m_window->GetWidth(), m_window->GetHeight());
    return true;
}

void Application::Shutdown() {
    m_renderer.reset();
    m_window->Shutdown();
    FBZZ_LOG_INFO("Application 終了 (フレーム数: %llu)", Time::FrameCount());
}

void Application::Run() {
    if (!Init()) return;

    while (m_isRunning) {
        Time::Tick();
        input::Input::Update();

        m_window->PollEvents();
        if (m_window->ShouldClose()) {
            Quit();
            break;
        }

        m_renderer->BeginFrame();
        m_renderer->Clear({ 0.10f, 0.15f, 0.25f, 1.0f });
        m_renderer->EndFrame();
    }

    Shutdown();
}

void Application::Quit() {
    m_isRunning = false;
}

} // namespace fbzz::core
