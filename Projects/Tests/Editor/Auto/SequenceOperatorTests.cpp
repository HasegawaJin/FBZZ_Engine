/// @file    SequenceOperatorTests.cpp
/// @brief   非表示タイムラインの原子的編集・履歴・保存・再生を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Panels/SequencePanel.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/ModalDialog.hpp>
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
        editor::AssetDirtyRegistry::DiscardAll();
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
/// @brief Hidden document edits support history and save.
TEST_F(SequenceOperatorTest, HiddenDocumentEditsSupportHistoryAndSave)
{
    ASSERT_TRUE(Duration(3.0f).ok); ASSERT_TRUE(Invoke("sequence.undo").ok);
    EXPECT_DOUBLE_EQ(Invoke("sequence.inspect").data.Find("duration")->AsDouble(), 2.0);
    ASSERT_TRUE(Invoke("sequence.redo").ok); ASSERT_TRUE(Invoke("sequence.save").ok);
    asset::SequenceAsset loaded; ASSERT_TRUE(asset::LoadSequenceAsset(File("Assets/Test.sequence").generic_string(), loaded));
    EXPECT_DOUBLE_EQ(loaded.duration, 3.0); EXPECT_FALSE(m_panel.visible);
}
/// @brief Unknown fields and negative time reject without changing document.
TEST_F(SequenceOperatorTest, UnknownFieldsAndNegativeTimeRejectWithoutChangingDocument)
{
    EXPECT_FALSE(Duration(3.0f, true).ok); EXPECT_FALSE(Duration(-1.0f).ok);
    const auto state = Invoke("sequence.inspect"); ASSERT_TRUE(state.ok);
    EXPECT_DOUBLE_EQ(state.data.Find("duration")->AsDouble(), 2.0);
    EXPECT_FALSE(state.data.Find("dirty")->AsBool()); EXPECT_EQ(state.data.Find("undoCount")->AsInt(), 0);
}
/// @brief Playback advances without rendering the panel.
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
/// @brief Close requires explicit discard for unsaved changes.
TEST_F(SequenceOperatorTest, CloseRequiresExplicitDiscardForUnsavedChanges)
{
    ASSERT_TRUE(Duration(3.0f).ok); EXPECT_FALSE(Invoke("sequence.close").ok);
    EXPECT_TRUE(Invoke("sequence.inspect").data.Find("dirty")->AsBool());
    editor::OpArgs close; close.Set("discard", true); ASSERT_TRUE(Invoke("sequence.close", close).ok);
    EXPECT_TRUE(Invoke("sequence.inspect").data.Find("path")->AsString().empty());
}
/// @brief アセットブラウザーから別の演出を開く際、未保存の編集は確認を閉じても残る。
TEST_F(SequenceOperatorTest, RequestedOpenKeepsUnsavedDocumentUntilConfirmed)
{
    asset::SequenceAsset next;
    next.name = "Other";
    next.duration = 4.0;
    const auto other = File("Assets/Other.sequence").generic_string();
    ASSERT_TRUE(asset::SaveSequenceAsset(other, next));
    ASSERT_TRUE(Duration(3.0f).ok);
    Context().operators = &m_registry;
    Context().undoStack = &m_undo;

    auto* previous = ImGui::GetCurrentContext();
    auto* imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = { 1280.0f, 900.0f };
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const auto draw = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({ 1000.0f, 800.0f });
        m_panel.OnRender(Context());
        editor::ModalDialog::OnRender();
        ImGui::EndFrame();
    };

    m_panel.visible = true;
    m_panel.RequestOpen(other);
    draw();
    io.AddKeyEvent(ImGuiKey_Escape, true);
    draw();
    ImGui::DestroyContext(imgui);
    ImGui::SetCurrentContext(previous);

    const auto state = Invoke("sequence.inspect");
    ASSERT_TRUE(state.ok);
    EXPECT_DOUBLE_EQ(state.data.Find("duration")->AsDouble(), 3.0);
    EXPECT_TRUE(state.data.Find("dirty")->AsBool());
}
/// @brief Hidden preview feeds the player and release restores its ownership.
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
