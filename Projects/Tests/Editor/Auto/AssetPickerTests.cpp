/// @file    AssetPickerTests.cpp
/// @brief   ピッカーが破棄済みの編集用コピーへ書かず、選択結果を生存中の欄へ一度だけ返すことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-13
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <memory>
#include <string>

namespace fbzz::tests {

class AssetPickerTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        m_previous = ImGui::GetCurrentContext();
        m_imgui = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_imgui);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = { 1000.0f, 800.0f };
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    void TearDown() override
    {
        for (int i = 0; i < 3; ++i) {
            BeginFrame();
            editor::widgets::DrawAssetPickerModal();
            editor::widgets::DrawAssetPickerModal(nullptr, nullptr,
                editor::widgets::AssetPickerHost::FLUID_EDITOR);
            EndFrame();
        }
        ImGui::DestroyContext(m_imgui);
        ImGui::SetCurrentContext(m_previous);
        EditorFixture::TearDown();
    }

    void BeginFrame()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0.0f, 0.0f });
        ImGui::SetNextWindowSize({ 900.0f, 700.0f });
        ImGui::Begin("Asset picker regression");
        m_popupId = ImGui::GetID("##asset_picker");
    }

    void EndFrame()
    {
        ImGui::End();
        ImGui::Render();
    }

    void Activate(ImGuiID id)
    {
        m_imgui->NavActivateId = id;
        m_imgui->NavActivateDownId = id;
        m_imgui->NavActivatePressedId = id;
        m_imgui->NavInputSource = ImGuiInputSource_Keyboard;
    }

    bool Field(std::string& path, bool browse = false, int owner = 0,
               editor::widgets::AssetPickerHost host = editor::widgets::AssetPickerHost::INSPECTOR)
    {
        ImGui::PushID(owner);
        if (browse) {
            ImGui::PushID("Texture");
            Activate(ImGui::GetID("..."));
            ImGui::PopID();
        }
        const bool changed = editor::widgets::AssetPathField(
            "Texture", path, ".png", ProjectRoot().generic_string(), host);
        ImGui::PopID();
        return changed;
    }

    ImGuiID NoneId() const
    {
        for (ImGuiWindow* window : m_imgui->Windows) {
            if (window->ParentWindow != nullptr && window->ParentWindow->PopupId == m_popupId
                && window->ChildId == window->ParentWindow->GetID("##list"))
                return window->GetID("(none)");
        }
        return 0;
    }

    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_imgui = nullptr;
    ImGuiID m_popupId = 0;
};

TEST_F(AssetPickerTest, DestroyedWorkingCopyReceivesSelectionOnNextDraw)
{
    BeginFrame();
    auto temporary = std::make_unique<std::string>("Assets/Textures/Mask.png");
    EXPECT_FALSE(Field(*temporary, true));
    temporary.reset();
    editor::widgets::DrawAssetPickerModal();
    const ImGuiID none = NoneId();
    EndFrame();
    ASSERT_NE(none, 0u);

    BeginFrame();
    Activate(none);
    editor::widgets::DrawAssetPickerModal();
    std::string nextCopy = "Assets/Textures/Mask.png";
    EXPECT_TRUE(Field(nextCopy));
    EXPECT_TRUE(nextCopy.empty());
    EndFrame();

    BeginFrame();
    EXPECT_FALSE(Field(nextCopy));
    editor::widgets::DrawAssetPickerModal();
    EndFrame();
}

TEST_F(AssetPickerTest, SelectionDoesNotReachAnotherOwnerOrReturnAfterOwnerDisappears)
{
    BeginFrame();
    std::string original = "Assets/Textures/Mask.png";
    EXPECT_FALSE(Field(original, true));
    editor::widgets::DrawAssetPickerModal();
    const ImGuiID none = NoneId();
    EndFrame();
    ASSERT_NE(none, 0u);

    BeginFrame();
    Activate(none);
    editor::widgets::DrawAssetPickerModal();
    std::string other = "Assets/Textures/Other.png";
    EXPECT_FALSE(Field(other, false, 1));
    EXPECT_EQ(other, "Assets/Textures/Other.png");
    EndFrame();

    BeginFrame();
    EXPECT_FALSE(Field(original));
    EXPECT_EQ(original, "Assets/Textures/Mask.png");
    editor::widgets::DrawAssetPickerModal();
    EndFrame();
}

TEST_F(AssetPickerTest, FluidEditorPickerIsDrawnByFluidEditorHost)
{
    BeginFrame();
    std::string path = "Assets/Textures/Mask.png";
    EXPECT_FALSE(Field(path, true, 0, editor::widgets::AssetPickerHost::FLUID_EDITOR));
    editor::widgets::DrawAssetPickerModal();
    EXPECT_EQ(NoneId(), 0u);
    editor::widgets::DrawAssetPickerModal(nullptr, nullptr,
        editor::widgets::AssetPickerHost::FLUID_EDITOR);
    EXPECT_NE(NoneId(), 0u);
    EndFrame();
}

}
