// FBZZ Engine
// WindowManualTests.cpp | GoogleTest ManualTest
// 実ウィンドウ生成とメッセージポンプを開発者が目視確認する。
#include <gtest/gtest.h>

#include <Engine/Core/Window.hpp>

#include <chrono>
#include <thread>

namespace fbzz::tests {

TEST(WindowManualTest, OpensAndPumpsAWin32Window)
{
    core::Window window;
    core::Window::Config config;
    config.title = L"FBZZ Core Window ManualTest";
    config.width = 640;
    config.height = 360;

    ASSERT_TRUE(window.Initialize(config));
    EXPECT_NE(window.GetHandle(), nullptr);
    EXPECT_EQ(window.GetWidth(), 640u);
    EXPECT_EQ(window.GetHeight(), 360u);

    // WHY: CTest から自動実行せず、開発者が表示と終了操作を確認できる時間だけポンプする。
    for (int frame = 0; frame < 120 && !window.ShouldClose(); ++frame) {
        window.PollEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    window.Shutdown();
}

} // namespace fbzz::tests
