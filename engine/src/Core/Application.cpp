// FBZZ Engine
// Application.cpp | fbzz::core
// エンジンのエントリポイントとメインループ
#include "engine/Core/Application.hpp"
#include "engine/Core/Logger.hpp"
#include "engine/Core/Time.hpp"
#include "engine/Input/Input.hpp"
#include "../Renderer/Platform/DX11/DX11Renderer.hpp"

namespace fbzz::core {

Application& Application::Get() {
    static Application instance;
    return instance;
}

void Application::Run() {
    Window::Config windowConfig;
    m_window = std::make_unique<Window>();
    if (!m_window->Initialize(windowConfig))
        return;

    input::Input::Init();

    auto dx11 = std::make_unique<renderer::DX11Renderer>();
    if (!dx11->Init(m_window->GetHandle(), m_window->GetWidth(), m_window->GetHeight()))
        return;
    m_renderer = std::move(dx11);

    m_window->SetResizeCallback([this](uint32_t w, uint32_t h) {
        m_renderer->Resize(w, h);
    });

    FBZZ_LOG_INFO("Application 起動: %ux%u", m_window->GetWidth(), m_window->GetHeight());

    while (m_isRunning) {
        Time::Tick();
        input::Input::Update();

        m_window->PollEvents();
        if (m_window->ShouldClose()) {
            Quit();
            break;
        }

        // TODO: scene.Update(Time::DeltaTime())
        // TODO: physicsWorld.Step(Time::DeltaTime())
        m_renderer->BeginFrame();
        m_renderer->Clear({ 0.10f, 0.15f, 0.25f, 1.0f });  // ネイビーブルー (Step1 動作確認用)
        m_renderer->EndFrame();
    }

    m_renderer.reset();
    m_window->Shutdown();
    FBZZ_LOG_INFO("Application 終了 (フレーム数: %llu)", Time::FrameCount());
}

void Application::Quit() {
    m_isRunning = false;
}

} // namespace fbzz::core
