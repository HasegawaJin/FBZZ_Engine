/// @file    HotkeyManagerTests.cpp
/// @brief   ホットキーが同じ入力を複数の編集操作へ渡さないことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-17
#include <TestKit/TestKit.hpp>
#include <Editor/Op/HotkeyManager.hpp>
#include <imgui.h>
#include <imgui_internal.h>

namespace fbzz::tests {
class HotkeyManagerTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        m_previous = ImGui::GetCurrentContext();
        m_context = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_context);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = { 800.0f, 600.0f };
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_context);
        ImGui::SetCurrentContext(m_previous);
    }

    void Press(editor::HotkeyManager& manager, ImGuiKey key)
    {
        ImGui::GetIO().AddKeyEvent(key, true);
        ImGui::NewFrame();
        manager.ProcessInput();
        ImGui::EndFrame();
    }

private:
    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_context = nullptr;
};

/// @brief 保存済みの競合は一度のキー入力で先に登録された使用可能な操作だけを実行する。
TEST_F(HotkeyManagerTest, ConflictingGlobalBindingsExecuteOnce)
{
    editor::HotkeyManager manager;
    int saveCount = 0;
    int playCount = 0;
    manager.Register({ .name = "Save", .imguiKey = ImGuiKey_K, .callback = [&] { ++saveCount; } });
    manager.Register({ .name = "Play", .imguiKey = ImGuiKey_K, .callback = [&] { ++playCount; } });

    Press(manager, ImGuiKey_K);

    EXPECT_EQ(saveCount, 1);
    EXPECT_EQ(playCount, 0);
}

/// @brief フォーカス中のウィジェットへ割り当て済みのキーは全体の操作を起動しない。
TEST_F(HotkeyManagerTest, KeyOwnedByFocusedWidgetDoesNotTriggerGlobalAction)
{
    editor::HotkeyManager manager;
    int count = 0;
    manager.Register({ .name = "Global Action", .imguiKey = ImGuiKey_K, .callback = [&] { ++count; } });

    ImGui::GetIO().AddKeyEvent(ImGuiKey_K, true);
    ImGui::NewFrame();
    ImGui::SetKeyOwner(ImGuiKey_K, ImGui::GetID("Focused Editor"));
    manager.ProcessInput();
    ImGui::EndFrame();

    EXPECT_EQ(count, 0);
}
} /// @note namespace fbzz::tests
