// FBZZ Engine
// Application.cpp | fbzz::core
// エンジンのエントリポイントとメインループ
#include "engine/Core/Application.hpp"

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

    while (m_isRunning) {
        m_window->PollEvents();
        if (m_window->ShouldClose()) {
            Quit();
            break;
        }

        // TODO: Input::Update()
        // TODO: scene.Update(dt)
        // TODO: physicsWorld.Step(dt)
        // TODO: renderer.BeginFrame() / EndFrame()
    }

    m_window->Shutdown();
}

void Application::Quit() {
    m_isRunning = false;
}

} // namespace fbzz::core
