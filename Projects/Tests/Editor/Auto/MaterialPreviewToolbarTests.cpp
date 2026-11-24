/// @file    MaterialPreviewToolbarTests.cpp
/// @brief   材質プレビューの形状選択とキャンバスがドック幅へ追従することを検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-23
#include <TestKit/TestKit.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Panels/MaterialPreview.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <imgui_internal.h>

namespace fbzz::tests {

class MaterialPreviewToolbarTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        m_previous = ImGui::GetCurrentContext();
        m_gui = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_gui);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = { 800.0f, 720.0f };
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        m_view.RegisterOperators(m_registry, "preview");
        m_material.shaderPath = FBZZ_SOURCE_DIR "/Assets/Shaders/Material/Surface/Lit.hlsl";
        ASSERT_EQ(editor::matpreview::DetectFlavor(m_material), editor::matpreview::Flavor::Surface);
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_gui);
        ImGui::SetCurrentContext(m_previous);
    }

    void Frame(float width, const char* activateNext = nullptr)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0.0f, 0.0f });
        ImGui::SetNextWindowSize({ width, 700.0f });
        ImGui::Begin("Material preview", nullptr, ImGuiWindowFlags_NoSavedSettings);
        m_availableWidth = ImGui::GetContentRegionAvail().x;
        const float left = ImGui::GetCursorScreenPos().x;
        m_view.Draw(m_context, m_material, 240.0f);
        m_canvas = ImGui::GetItemRectSize();
        m_contentWidth = ImGui::GetCurrentWindow()->DC.CursorMaxPos.x - left;
        if (activateNext) {
            ImGui::PushID(&m_view);
            ImGui::ActivateItemByID(ImGui::GetID(activateNext));
            ImGui::PopID();
        }
        ImGui::End();
        ImGui::Render();
    }

    std::string CurrentShape()
    {
        editor::OpContext op{ m_context, m_undo };
        const auto result = m_registry.Invoke("preview.inspect", op);
        return result.data.Find("shape")->AsString();
    }

    editor::EditorContext m_context;
    editor::MaterialPreviewView m_view;
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    asset::MaterialAssetAuthoring m_material;
    ImVec2 m_canvas;
    float m_availableWidth = 0;
    float m_contentWidth = 0;

private:
    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_gui = nullptr;
};

/// @brief 狭いドックでも各形状を直接選択でき、専用形状の材質は以前の選択を保持する。
TEST_F(MaterialPreviewToolbarTest, SelectsEachShapeDirectlyAndPreservesItForFixedGeometry)
{
    for (const char* shape : { "Cube", "Plane", "Cylinder", "Cone", "Torus", "Capsule", "Sphere" }) {
        Frame(180.0f, shape);
        Frame(180.0f);
        EXPECT_EQ(CurrentShape(), shape);
        EXPECT_LE(m_contentWidth, m_availableWidth + 1.0f);
    }
    m_material.renderPath = asset::RenderPath::Particle;
    Frame(180.0f, "Torus");
    Frame(180.0f);
    EXPECT_EQ(CurrentShape(), "Sphere");
}

/// @brief 横長の Inspector ではキャンバスが左右の余白まで使い、指定した高さを保つ。
TEST_F(MaterialPreviewToolbarTest, UsesFullInspectorWidthAtTheRequestedCanvasHeight)
{
    Frame(392.0f);
    EXPECT_FLOAT_EQ(m_canvas.x, m_availableWidth);
    EXPECT_FLOAT_EQ(m_canvas.y, 240.0f);
}

}
