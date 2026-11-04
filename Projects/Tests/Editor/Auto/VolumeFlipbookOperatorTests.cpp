/// @file    VolumeFlipbookOperatorTests.cpp
/// @brief   非表示の解析ベイク設定と失敗時の原子性を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Panels/VolumeFlipbookBakePanel.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/UndoStack.hpp>
namespace fbzz::tests {
class VolumeFlipbookOperatorTest : public testkit::EditorFixture {
protected:
    void SetUp() override { EditorFixture::SetUp(); m_panel.visible = false; m_panel.OnRegisterOperators(m_registry); }
    editor::OpResult Invoke(const char* id, const editor::OpArgs& args = {}) {
        editor::OpContext context{Context(), m_undo}; return m_registry.Invoke(id, context, args);
    }
    editor::VolumeFlipbookBakePanel m_panel; editor::OperatorRegistry m_registry; editor::UndoStack m_undo;
};
TEST_F(VolumeFlipbookOperatorTest, HiddenPanelCanConfigureNestedEmitterAndLook)
{
    auto patch = editor::OpData::MakeObject(); patch.Set("source.preset", editor::OpData("Emitter"));
    patch.Set("source.emitter.count", editor::OpData(42)); patch.Set("liquid.softness", editor::OpData(0.1f));
    patch.Set("baseName", editor::OpData("SmokeTest")); editor::OpArgs args; args.Set("settings", patch);
    const auto result = Invoke("volume.flipbook.configure", args); ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.data.Find("settings")->Find("source.emitter.count")->AsInt(), 42);
    EXPECT_EQ(result.data.Find("settings")->Find("baseName")->AsString(), "SmokeTest"); EXPECT_FALSE(m_panel.visible);
}
TEST_F(VolumeFlipbookOperatorTest, InvalidRampOrderingKeepsAllPreviousSettings)
{
    const auto before = Invoke("volume.flipbook.inspect");
    auto patch = editor::OpData::MakeObject(); patch.Set("source.frameCount", editor::OpData(8));
    patch.Set("albedoRamp.stops[0].position", editor::OpData(1.0f));
    editor::OpArgs args; args.Set("settings", patch); EXPECT_FALSE(Invoke("volume.flipbook.configure", args).ok);
    const auto after = Invoke("volume.flipbook.inspect");
    EXPECT_EQ(after.data.Find("settings")->Find("source.frameCount")->AsInt(), before.data.Find("settings")->Find("source.frameCount")->AsInt());
}
TEST_F(VolumeFlipbookOperatorTest, UnknownFieldsDoNotPartiallyStartPlayback)
{
    auto patch = editor::OpData::MakeObject(); patch.Set("playing", editor::OpData(true)); patch.Set("typo", editor::OpData(1));
    editor::OpArgs args; args.Set("settings", patch); EXPECT_FALSE(Invoke("volume.flipbook.control", args).ok);
    EXPECT_FALSE(Invoke("volume.flipbook.inspect").data.Find("playback")->Find("playing")->AsBool());
    EXPECT_FALSE(Invoke("volume.flipbook.bake").ok); EXPECT_EQ(Invoke("volume.flipbook.inspect").data.Find("job")->AsInt(), 0);
}
} /// @note namespace fbzz::tests
