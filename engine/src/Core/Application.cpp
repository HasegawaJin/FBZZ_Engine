#include "engine/Core/Application.hpp"
#include <iostream>

namespace fbzz::core {

Application& Application::Get() {
    static Application instance;
    return instance;
}

void Application::Run() {
    std::cout << "FBZZ Engine started." << std::endl;

    while (m_isRunning) {
        // TODO: Input::Update()
        // TODO: scene.Update(dt)
        // TODO: physicsWorld.Step(dt)
        // TODO: renderer.BeginFrame() / EndFrame()
        Quit();
    }
}

void Application::Quit() {
    m_isRunning = false;
}

} // namespace fbzz::core
