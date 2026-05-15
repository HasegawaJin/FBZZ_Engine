#pragma once

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
};

} // namespace fbzz::core
