/// @file    IblOperatorTests.cpp
/// @brief   IBL直接APIの設定検証とGPUジョブ要求の境界を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Editor/Panels/IblBakePanel.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <fstream>
namespace fbzz::tests {
class IblOperatorTest : public testkit::EditorFixture {
protected:
    void SetUp() override { EditorFixture::SetUp(); m_panel.visible = false; m_panel.OnRegisterOperators(m_registry); }
    editor::OpResult Invoke(const char* id, const editor::OpArgs& args = {}) {
        editor::OpContext context{Context(), m_undo}; return m_registry.Invoke(id, context, args);
    }
    editor::IblBakePanel m_panel; editor::OperatorRegistry m_registry; editor::UndoStack m_undo;
};
/// @brief Hidden panel configuration does not require renderer.
TEST_F(IblOperatorTest, HiddenPanelConfigurationDoesNotRequireRenderer)
{
    auto settings = editor::OpData::MakeObject(); settings.Set("sampleCount", editor::OpData(512));
    settings.Set("baseName", editor::OpData("Sky")); editor::OpArgs args; args.Set("settings", settings);
    const auto result = Invoke("ibl.configure", args); ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.data.Find("settings")->Find("sampleCount")->AsInt(), 512);
    EXPECT_FALSE(m_panel.visible); EXPECT_EQ(result.data.Find("state")->AsString(), "idle");
}
/// @brief Invalid partial settings do not apply other values.
TEST_F(IblOperatorTest, InvalidPartialSettingsDoNotApplyOtherValues)
{
    auto settings = editor::OpData::MakeObject(); settings.Set("sampleCount", editor::OpData(512));
    settings.Set("envCubemapSize", editor::OpData(123)); editor::OpArgs args; args.Set("settings", settings);
    EXPECT_FALSE(Invoke("ibl.configure", args).ok);
    const auto result = Invoke("ibl.inspect"); EXPECT_EQ(result.data.Find("settings")->Find("sampleCount")->AsInt(), 1024);
    EXPECT_EQ(result.data.Find("job")->AsInt(), 0);
}
/// @brief Missing renderer does not report a queued or successful bake.
TEST_F(IblOperatorTest, MissingRendererDoesNotReportAQueuedOrSuccessfulBake)
{
    EXPECT_FALSE(Invoke("ibl.bake").ok);
    const auto result = Invoke("ibl.inspect"); EXPECT_EQ(result.data.Find("job")->AsInt(), 0);
    EXPECT_EQ(result.data.Find("state")->AsString(), "idle");
}

/// @brief GUI で選んだ出力先で要求を受け付け、開始前に取り消せる。自動実行の出力先は Assets 配下に限る。
TEST_F(IblOperatorTest, InteractiveBakeOutsideAssetsCanBeCancelledBeforeStarting)
{
    testkit::ResourceTrackingRenderer renderer;
    Context().renderer = &renderer;
    Context().projectRoot = ProjectRoot().generic_string();
    std::ofstream(File("sky.hdr")) << "source";

    auto settings = editor::OpData::MakeObject();
    settings.Set("source", editor::OpData("sky.hdr"));
    settings.Set("outputDirectory", editor::OpData("Export"));
    settings.Set("baseName", editor::OpData("Sky"));
    editor::OpArgs args;
    args.Set("settings", settings);
    editor::OpContext automated{Context(), m_undo, editor::OpInvocationSource::AUTOMATION};
    EXPECT_EQ(m_registry.Invoke("ibl.bake", automated, args).errorCode, "BAD_PATH");

    const auto request = Invoke("ibl.bake", args);
    ASSERT_TRUE(request.ok) << request.message;
    EXPECT_EQ(request.data.Find("state")->AsString(), "queued");
    ASSERT_TRUE(Invoke("ibl.cancel").ok);
    m_panel.OnUpdate(Context(), 0.0f);
    const auto state = Invoke("ibl.inspect");
    EXPECT_EQ(state.data.Find("state")->AsString(), "cancelled");
    EXPECT_FALSE(std::filesystem::exists(File("Export")));
    Context().renderer = nullptr;
}
} /// @note namespace fbzz::tests
