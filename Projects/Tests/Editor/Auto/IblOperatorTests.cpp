/// @file    IblOperatorTests.cpp
/// @brief   IBL直接APIの設定検証とGPUジョブ要求の境界を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Panels/IblBakePanel.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/UndoStack.hpp>
namespace fbzz::tests {
class IblOperatorTest : public testkit::EditorFixture {
protected:
    void SetUp() override { EditorFixture::SetUp(); m_panel.visible = false; m_panel.OnRegisterOperators(m_registry); }
    editor::OpResult Invoke(const char* id, const editor::OpArgs& args = {}) {
        editor::OpContext context{Context(), m_undo}; return m_registry.Invoke(id, context, args);
    }
    editor::IblBakePanel m_panel; editor::OperatorRegistry m_registry; editor::UndoStack m_undo;
};
TEST_F(IblOperatorTest, HiddenPanelConfigurationDoesNotRequireRenderer)
{
    auto settings = editor::OpData::MakeObject(); settings.Set("sampleCount", editor::OpData(512));
    settings.Set("baseName", editor::OpData("Sky")); editor::OpArgs args; args.Set("settings", settings);
    const auto result = Invoke("ibl.configure", args); ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.data.Find("settings")->Find("sampleCount")->AsInt(), 512);
    EXPECT_FALSE(m_panel.visible); EXPECT_EQ(result.data.Find("state")->AsString(), "idle");
}
TEST_F(IblOperatorTest, InvalidPartialSettingsDoNotApplyOtherValues)
{
    auto settings = editor::OpData::MakeObject(); settings.Set("sampleCount", editor::OpData(512));
    settings.Set("envCubemapSize", editor::OpData(123)); editor::OpArgs args; args.Set("settings", settings);
    EXPECT_FALSE(Invoke("ibl.configure", args).ok);
    const auto result = Invoke("ibl.inspect"); EXPECT_EQ(result.data.Find("settings")->Find("sampleCount")->AsInt(), 1024);
    EXPECT_EQ(result.data.Find("job")->AsInt(), 0);
}
TEST_F(IblOperatorTest, MissingRendererDoesNotReportAQueuedOrSuccessfulBake)
{
    EXPECT_FALSE(Invoke("ibl.bake").ok);
    const auto result = Invoke("ibl.inspect"); EXPECT_EQ(result.data.Find("job")->AsInt(), 0);
    EXPECT_EQ(result.data.Find("state")->AsString(), "idle");
}
} /// @note namespace fbzz::tests
