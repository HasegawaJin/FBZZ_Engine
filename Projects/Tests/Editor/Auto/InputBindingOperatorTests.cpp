/// @file    InputBindingOperatorTests.cpp
/// @brief   入力設定の直接編集・保存・再読込・待機状態とUndo競合を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Util/InputBindingsEditing.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Input/InputActionMap.hpp>
#include <filesystem>

namespace fbzz::tests {
class InputBindingOperatorTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        m_original = editor::CaptureInputBindings();
        m_enabled = input::InputActionMap::IsEnabled();
        input::InputActionMap::LoadDefaults();
        Context().projectRoot = ProjectRoot().generic_string();
        editor::RegisterInputBindingOperators(m_registry);
    }
    void TearDown() override
    {
        m_undo.Clear();
        input::InputActionMap::Clear();
        for (const auto& axis : m_original.axes) input::InputActionMap::AddAxis(axis);
        for (const auto& action : m_original.actions) input::InputActionMap::AddAction(action);
        input::InputActionMap::SetEnabled(m_enabled);
        EditorFixture::TearDown();
    }
    editor::OpResult Invoke(const char* id, const editor::OpArgs& args = {})
    {
        editor::OpContext context{Context(), m_undo};
        return m_registry.Invoke(id, context, args);
    }
    editor::OpData ActionPatch(int code)
    {
        auto binding = editor::OpData::MakeObject();
        binding.Set("source", editor::OpData("Key")); binding.Set("code", editor::OpData(code));
        auto bindings = editor::OpData::MakeArray(); bindings.Push(binding);
        auto action = editor::OpData::MakeObject();
        action.Set("name", editor::OpData("TestAction")); action.Set("bindings", bindings);
        auto actions = editor::OpData::MakeArray(); actions.Push(action);
        auto patch = editor::OpData::MakeObject(); patch.Set("action", actions);
        return patch;
    }
    editor::OpResult Configure(const editor::OpData& patch)
    {
        editor::OpArgs args; args.Set("settings", patch);
        return Invoke("input.bindings.configure", args);
    }
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    input::InputBindingsDocument m_original;
    bool m_enabled = false;
};

TEST_F(InputBindingOperatorTest, EditReplacesActionsKeepsAxesAndSupportsUndoRedo)
{
    const auto before = editor::CaptureInputBindings();

    const auto result = Configure(ActionPatch(74));

    ASSERT_TRUE(result.ok) << result.message;
    ASSERT_NE(input::InputActionMap::FindAction("TestAction"), nullptr);
    ASSERT_EQ(input::InputActionMap::FindAction("TestAction")->bindings.size(), 1u);
    EXPECT_EQ(input::InputActionMap::FindAction("TestAction")->bindings[0].code, 74u);
    EXPECT_EQ(input::InputActionMap::GetAxes().size(), before.axes.size());
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(editor::CaptureInputBindings().ToToml(), before.ToToml());
    ASSERT_TRUE(m_undo.Redo());
    EXPECT_NE(input::InputActionMap::FindAction("TestAction"), nullptr);
}

TEST_F(InputBindingOperatorTest, InvalidKeyCodeLeavesAllBindingsAndHistoryUnchanged)
{
    const std::string before = editor::CaptureInputBindings().ToToml();

    const auto result = Configure(ActionPatch(256));

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(editor::CaptureInputBindings().ToToml(), before);
    EXPECT_FALSE(m_undo.CanUndo());
}

TEST_F(InputBindingOperatorTest, UnknownNestedFieldsRejectTheWholeEdit)
{
    const std::string before = editor::CaptureInputBindings().ToToml();
    auto patch = ActionPatch(74);
    patch.Set("unknown", editor::OpData(true));

    const auto result = Configure(patch);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(editor::CaptureInputBindings().ToToml(), before);
}

TEST_F(InputBindingOperatorTest, EmptyArraysAndExplicitDefaultValuesCanBeAuthored)
{
    auto axis = editor::OpData::MakeObject();
    axis.Set("name", editor::OpData("TestAxis")); axis.Set("raw", editor::OpData(false));
    axis.Set("analog", editor::OpData::MakeArray());
    axis.Set("positive", editor::OpData::MakeArray());
    axis.Set("negative", editor::OpData::MakeArray());
    auto axes = editor::OpData::MakeArray(); axes.Push(axis);
    auto patch = editor::OpData::MakeObject(); patch.Set("axis", axes);

    const auto result = Configure(patch);

    ASSERT_TRUE(result.ok) << result.message;
    const auto* actual = input::InputActionMap::FindAxis("TestAxis");
    ASSERT_NE(actual, nullptr);
    EXPECT_FALSE(actual->raw);
    EXPECT_TRUE(actual->positive.empty());
    EXPECT_TRUE(actual->negative.empty());
    EXPECT_TRUE(actual->analog.empty());
}

TEST_F(InputBindingOperatorTest, SaveAndReloadWorkWithoutOpeningTheSettingsPanel)
{
    ASSERT_TRUE(Configure(ActionPatch(74)).ok);
    ASSERT_TRUE(Invoke("input.bindings.save").ok);
    EXPECT_TRUE(std::filesystem::exists(File("ProjectSettings/Input.inputactions")));
    ASSERT_TRUE(Invoke("input.bindings.reset").ok);
    ASSERT_EQ(input::InputActionMap::FindAction("TestAction"), nullptr);

    const auto result = Invoke("input.bindings.reload");

    ASSERT_TRUE(result.ok) << result.message;
    ASSERT_NE(input::InputActionMap::FindAction("TestAction"), nullptr);
    EXPECT_EQ(input::InputActionMap::FindAction("TestAction")->bindings[0].code, 74u);
}

TEST_F(InputBindingOperatorTest, PendingRebindCanBeInspectedCancelledAndCannotBeSaved)
{
    ASSERT_TRUE(Configure(ActionPatch(74)).ok);
    editor::OpArgs args;
    args.Set("kind", std::string("action")); args.Set("name", std::string("TestAction"));
    ASSERT_TRUE(Invoke("input.bindings.rebind", args).ok);

    const auto inspected = Invoke("input.bindings.inspect");
    const auto save = Invoke("input.bindings.save");

    ASSERT_TRUE(inspected.ok);
    ASSERT_NE(inspected.data.Find("rebinding"), nullptr);
    EXPECT_TRUE(inspected.data.Find("rebinding")->AsBool());
    EXPECT_FALSE(save.ok);
    EXPECT_EQ(save.errorCode, "INPUT_REBIND_PENDING");
    ASSERT_TRUE(Invoke("input.bindings.cancel_rebind").ok);
    EXPECT_FALSE(input::InputActionMap::IsRebinding());
}

TEST_F(InputBindingOperatorTest, OutOfBandBindingEditPreventsUndoFromReplacingIt)
{
    ASSERT_TRUE(Configure(ActionPatch(74)).ok);
    input::InputAction external; external.name = "External";
    ASSERT_TRUE(input::InputActionMap::AddAction(external));

    EXPECT_FALSE(m_undo.Undo());

    EXPECT_NE(input::InputActionMap::FindAction("External"), nullptr);
    EXPECT_EQ(m_undo.GetCursor(), 1u);
}
} /// @note namespace fbzz::tests
