/// @file    PropertyControlsTests.cpp
/// @brief   プロパティの値列と狭いドックでの折り返しを検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-17
#include <TestKit/TestKit.hpp>
#include <Editor/Inspector/Widgets/PropertyControls.hpp>
#include <imgui_internal.h>

namespace fbzz::tests {
class PropertyControlsTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        m_previous = ImGui::GetCurrentContext();
        m_imgui = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_imgui);
        auto& io = ImGui::GetIO();
        io.DisplaySize = { 960.0f, 720.0f };
        io.IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_imgui);
        ImGui::SetCurrentContext(m_previous);
    }

private:
    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_imgui = nullptr;
};

namespace {

constexpr ImU32 VALUE_COLOR = IM_COL32(247, 193, 97, 255);

float RenderedValueRightEdge(const ImDrawList& drawList, int firstVertex)
{
    float rightmost = 0.0f;
    for (int i = firstVertex; i < drawList.VtxBuffer.Size; ++i) {
        const auto& vertex = drawList.VtxBuffer[i];
        if (vertex.col == VALUE_COLOR) rightmost = (std::max)(rightmost, vertex.pos.x);
    }
    return rightmost;
}

} /// @note namespace

/// @brief 手書きの数値と真偽値は同じ X 座標に始まる。
TEST_F(PropertyControlsTest, DifferentControlsShareTheValueColumn)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowSize({ 420.0f, 270.0f });
    ImGui::Begin("Properties");
    float value = 0.0f;
    bool enabled = false;

    editor::property::DragFloat("Near", &value);
    const float numberX = ImGui::GetItemRectMin().x;
    editor::property::Checkbox("A much longer property label than Near", &enabled);
    const float toggleX = ImGui::GetItemRectMin().x;

    ImGui::End();
    ImGui::EndFrame();
    EXPECT_FLOAT_EQ(numberX, toggleX);
}

/// @brief 狭いドックでは値をラベルの次行へ置き、入力欄をパネル内に収める。
TEST_F(PropertyControlsTest, NarrowPanePlacesValueUnderLabel)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowSize({ 125.0f, 270.0f });
    ImGui::Begin("Narrow Properties");
    const float left = ImGui::GetCursorScreenPos().x;
    const float right = left + ImGui::GetContentRegionAvail().x;
    const float labelY = ImGui::GetCursorScreenPos().y;
    float value = 0.0f;

    editor::property::DragFloat("Long property name", &value);

    const auto minimum = ImGui::GetItemRectMin();
    const auto maximum = ImGui::GetItemRectMax();
    ImGui::End();
    ImGui::EndFrame();
    EXPECT_FLOAT_EQ(minimum.x, left);
    EXPECT_GT(minimum.y, labelY);
    EXPECT_LE(maximum.x, right);
}

/// @brief Transform の Scale には軸ラベルの隣に入力欄があり、数値は欄の右端に揃う。
TEST_F(PropertyControlsTest, TransformScaleValuesAlignAtTheRightEdge)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowSize({ 540.0f, 270.0f });
    ImGui::Begin("Transform Properties");

    const auto& drawList = *ImGui::GetWindowDrawList();
    const int firstVertex = drawList.VtxBuffer.Size;
    ImGui::PushStyleColor(ImGuiCol_Text, VALUE_COLOR);
    ImGui::SetNextItemWidth(420.0f);
    math::Vector3 scale{ 1.0f, 10.0f, 100.0f };
    bool uniform = false;
    editor::widgets::DragScaleAxes("##scale", scale, uniform);
    ImGui::PopStyleColor();

    const float rightmostValue = RenderedValueRightEdge(drawList, firstVertex);
    const auto& style = ImGui::GetStyle();
    const float valueRight = ImGui::GetItemRectMax().x - ImGui::GetFrameHeight()
        - style.ItemSpacing.x - style.FramePadding.x;
    ImGui::End();
    ImGui::EndFrame();
    EXPECT_NEAR(rightmostValue, valueRight, 4.0f);
}

/// @brief UI Transform の回転は直接入力に切り替えても同じ右端に並ぶ。
TEST_F(PropertyControlsTest, TemporaryScalarInputKeepsTheValueRightAligned)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowSize({ 420.0f, 270.0f });
    ImGui::Begin("UI Transform Properties");

    const char* id = "##rz";
    const auto itemId = ImGui::GetID(id);
    GImGui->NavActivateId = itemId;
    GImGui->NavActivateFlags = ImGuiActivateFlags_PreferInput;

    const auto& drawList = *ImGui::GetWindowDrawList();
    const int firstVertex = drawList.VtxBuffer.Size;
    ImGui::PushStyleColor(ImGuiCol_Text, VALUE_COLOR);
    ImGui::SetNextItemWidth(220.0f);
    float rotation = -12.5f;
    ImGui::DragFloat(id, &rotation, 0.5f, -360.0f, 360.0f, "Z %.1f deg", ImGuiSliderFlags_AlignValueRight);
    ImGui::PopStyleColor();

    const bool editing = ImGui::TempInputIsActive(itemId);
    const float renderedRight = RenderedValueRightEdge(drawList, firstVertex);
    const float expectedRight = ImGui::GetItemRectMax().x - ImGui::GetStyle().FramePadding.x;
    ImGui::End();
    ImGui::EndFrame();
    EXPECT_TRUE(editing);
    EXPECT_NEAR(renderedRight, expectedRight, 4.0f);
}
} /// @note namespace fbzz::tests
