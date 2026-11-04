/// @file    RenderPassOperatorTests.cpp
/// @brief   非表示の描画パス捕捉と設定検証を公開操作から検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Panels/RenderPassViewerPanel.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/UndoStack.hpp>
namespace fbzz::tests {
class RenderPassOperatorTest : public testkit::EditorFixture {
protected:
    void SetUp() override { EditorFixture::SetUp(); m_panel.visible = false; m_panel.OnRegisterOperators(m_registry); }
    editor::OpResult Invoke(const char* id, const editor::OpArgs& args = {}) {
        editor::OpContext context{Context(), m_undo}; return m_registry.Invoke(id, context, args);
    }
    editor::OpResult Configure(const editor::OpData& settings) { editor::OpArgs args; args.Set("settings", settings); return Invoke("renderpass.configure", args); }
    editor::RenderPassViewerPanel m_panel; editor::OperatorRegistry m_registry; editor::UndoStack m_undo;
};
TEST_F(RenderPassOperatorTest, HiddenPanelCanCaptureTheExplicitViewAndReleaseIt)
{
    auto settings = editor::OpData::MakeObject(); settings.Set("enabled", editor::OpData(true)); settings.Set("view", editor::OpData("game"));
    ASSERT_TRUE(Configure(settings).ok); m_panel.PrepareFrame();
    EXPECT_NE(m_panel.CaptureForView(true), nullptr); EXPECT_EQ(m_panel.CaptureForView(false), nullptr); EXPECT_FALSE(m_panel.visible);
    settings = editor::OpData::MakeObject(); settings.Set("enabled", editor::OpData(false)); ASSERT_TRUE(Configure(settings).ok);
    m_panel.PrepareFrame(); EXPECT_EQ(m_panel.CaptureForView(true), nullptr);
}
TEST_F(RenderPassOperatorTest, InvalidPartialSettingsKeepTheWholeRequestUnchanged)
{
    auto settings = editor::OpData::MakeObject(); settings.Set("enabled", editor::OpData(true)); settings.Set("view", editor::OpData("missing"));
    EXPECT_FALSE(Configure(settings).ok);
    const auto result = Invoke("renderpass.inspect"); ASSERT_TRUE(result.ok);
    EXPECT_FALSE(result.data.Find("settings")->Find("enabled")->AsBool()); EXPECT_EQ(m_panel.CaptureForView(false), nullptr);
}
TEST_F(RenderPassOperatorTest, UnknownNestedViewFieldsAndEmptyRangesAreRejected)
{
    auto settings = editor::OpData::MakeObject(); auto view = editor::OpData::MakeObject();
    view.Set("unknown", editor::OpData(true)); settings.Set("viewSettings", view); EXPECT_FALSE(Configure(settings).ok);
    view = editor::OpData::MakeObject(); view.Set("rangeMin", editor::OpData(1.0f)); view.Set("rangeMax", editor::OpData(1.0f));
    settings.Set("viewSettings", view); EXPECT_FALSE(Configure(settings).ok);
}
} /// @note namespace fbzz::tests
