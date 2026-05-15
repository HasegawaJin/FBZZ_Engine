// FBZZ Engine
// Application.hpp | fbzz::core
// エンジンのエントリポイントとメインループ
#pragma once
#include "engine/Core/Window.hpp"
#include <memory>

namespace fbzz::core {

class Application {
public:
    static Application& Get();

    void Run();
    void Quit();

    bool IsRunning() const { return m_isRunning; }

private:
    Application() = default;

    bool m_isRunning = true;
    std::unique_ptr<Window> m_window;
};

} // namespace fbzz::core
