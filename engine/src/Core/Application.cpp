// FBZZ Engine
// Application.cpp | fbzz::core
// エンジンのエントリポイントとメインループ
#include "engine/Core/Application.hpp"
#include "engine/Core/Logger.hpp"
#include "engine/Core/Time.hpp"
#include "engine/Input/Input.hpp"

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
        // TODO: renderer.BeginFrame() / EndFrame()
    }

    m_window->Shutdown();
    FBZZ_LOG_INFO("Application 終了 (フレーム数: %llu)", Time::FrameCount());
}

void Application::Quit() {
    m_isRunning = false;
}

} // namespace fbzz::core
