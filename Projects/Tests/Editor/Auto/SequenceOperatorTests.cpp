/// @file    SequenceOperatorTests.cpp
/// @brief   非表示タイムラインの原子的編集・履歴・保存・再生を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Panels/SequencePanel.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/SequenceAsset.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/SequencePlayerComponent.hpp>

namespace fbzz::tests {
class SequenceOperatorTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp(); Context().projectRoot = ProjectRoot().generic_string();
        asset::SequenceAsset sequence; sequence.name = "Example"; sequence.duration = 2.0;
        ASSERT_TRUE(asset::SaveSequenceAsset(File("Assets/Test.sequence").generic_string(), sequence));
        m_panel.visible = false; m_panel.OnRegisterOperators(m_registry);
        editor::OpArgs args; args.Set("path", std::string("Assets/Test.sequence"));
        ASSERT_TRUE(Invoke("sequence.open", args).ok);
    }
    void TearDown() override
    {
        editor::OpArgs args; args.Set("discard", true); static_cast<void>(Invoke("sequence.close", args));
        m_registry.Clear(); m_panel.OnShutdown(); EditorFixture::TearDown();
    }
    editor::OpResult Invoke(const char* id, const editor::OpArgs& args = {})
    {
        editor::OpContext context{Context(), m_undo}; return m_registry.Invoke(id, context, args);
    }
    editor::OpResult Duration(float seconds, bool invalid = false)
    {
        auto document = editor::OpData::MakeObject(); auto sequence = editor::OpData::MakeObject();
        sequence.Set("duration", editor::OpData(seconds)); if (invalid) sequence.Set("missing", editor::OpData(true));
        document.Set("sequence", sequence); editor::OpArgs args; args.Set("document", document);
        return Invoke("sequence.configure", args);
    }
    editor::SequencePanel m_panel;
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
};
TEST_F(SequenceOperatorTest, HiddenDocumentEditsSupportHistoryAndSave)
{
    ASSERT_TRUE(Duration(3.0f).ok); ASSERT_TRUE(Invoke("sequence.undo").ok);
    EXPECT_DOUBLE_EQ(Invoke("sequence.inspect").data.Find("duration")->AsDouble(), 2.0);
    ASSERT_TRUE(Invoke("sequence.redo").ok); ASSERT_TRUE(Invoke("sequence.save").ok);
    asset::SequenceAsset loaded; ASSERT_TRUE(asset::LoadSequenceAsset(File("Assets/Test.sequence").generic_string(), loaded));
    EXPECT_DOUBLE_EQ(loaded.duration, 3.0); EXPECT_FALSE(m_panel.visible);
}
TEST_F(SequenceOperatorTest, UnknownFieldsAndNegativeTimeRejectWithoutChangingDocument)
{
    EXPECT_FALSE(Duration(3.0f, true).ok); EXPECT_FALSE(Duration(-1.0f).ok);
    const auto state = Invoke("sequence.inspect"); ASSERT_TRUE(state.ok);
    EXPECT_DOUBLE_EQ(state.data.Find("duration")->AsDouble(), 2.0);
    EXPECT_FALSE(state.data.Find("dirty")->AsBool()); EXPECT_EQ(state.data.Find("undoCount")->AsInt(), 0);
}
TEST_F(SequenceOperatorTest, PlaybackAdvancesWithoutRenderingThePanel)
{
    editor::OpArgs play; play.Set("action", std::string("play")); play.Set("preview", false);
    ASSERT_TRUE(Invoke("sequence.control", play).ok);
    m_panel.OnUpdate(Context(), 0.5f);
    auto state = Invoke("sequence.inspect"); EXPECT_DOUBLE_EQ(state.data.Find("time")->AsDouble(), 0.5);
    m_panel.OnUpdate(Context(), 2.0f);
    state = Invoke("sequence.inspect"); EXPECT_DOUBLE_EQ(state.data.Find("time")->AsDouble(), 2.0);
    EXPECT_FALSE(state.data.Find("playing")->AsBool()); EXPECT_FALSE(m_panel.visible);
}
TEST_F(SequenceOperatorTest, CloseRequiresExplicitDiscardForUnsavedChanges)
{
    ASSERT_TRUE(Duration(3.0f).ok); EXPECT_FALSE(Invoke("sequence.close").ok);
    EXPECT_TRUE(Invoke("sequence.inspect").data.Find("dirty")->AsBool());
    editor::OpArgs close; close.Set("discard", true); ASSERT_TRUE(Invoke("sequence.close", close).ok);
    EXPECT_TRUE(Invoke("sequence.inspect").data.Find("path")->AsString().empty());
}
TEST_F(SequenceOperatorTest, HiddenPreviewFeedsThePlayerAndReleaseRestoresItsOwnership)
{
    auto& world = AttachScene(); auto& owner = world.CreateGameObject("Sequence Target");
    auto& player = owner.AddComponent<scene::SequencePlayerComponent>();
    editor::OpArgs seek; seek.Set("action", std::string("seek")); seek.Set("preview", true);
    seek.Set("target", owner.instanceId); seek.Set("time", 1.0f);
    ASSERT_TRUE(Invoke("sequence.control", seek).ok);
    ASSERT_NE(player.authoringSequence, nullptr); EXPECT_FLOAT_EQ(player.editorScrubTime, 1.0f); EXPECT_FALSE(m_panel.visible);
    m_panel.OnUpdate(Context(), 0.25f); EXPECT_FLOAT_EQ(player.editorScrubTime, 1.0f);
    editor::OpArgs release; release.Set("action", std::string("release")); ASSERT_TRUE(Invoke("sequence.control", release).ok);
    EXPECT_EQ(player.authoringSequence, nullptr); EXPECT_LT(player.editorScrubTime, 0.0f);
}
} /// @note namespace fbzz::tests
