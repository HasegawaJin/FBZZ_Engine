/// @file    ProjectSettingsOperatorTests.cpp
/// @brief   設定編集の原子性・入力分離・履歴競合を公開Operatorから検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Input/InputActionMap.hpp>

namespace fbzz::tests {

class ProjectSettingsOperatorTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        m_targetFps = Time::targetFps;
        m_actions = input::InputActionMap::GetActions();
        m_axes = input::InputActionMap::GetAxes();
        m_inputEnabled = input::InputActionMap::IsEnabled();
        Context().projectRoot = ProjectRoot().generic_string();
        Context().projectSettings = ProjectSettings::Default();
        editor::RegisterProjectSettingsOperators(m_registry);
    }
    void TearDown() override
    {
        m_undo.Clear();
        Time::targetFps = m_targetFps;
        input::InputActionMap::Clear();
        for (const auto& action : m_actions) input::InputActionMap::AddAction(action);
        for (const auto& axis : m_axes) input::InputActionMap::AddAxis(axis);
        input::InputActionMap::SetEnabled(m_inputEnabled);
        EditorFixture::TearDown();
    }
    editor::OpResult Configure(const editor::OpData& patch)
    {
        editor::OpArgs args; args.Set("settings", patch);
        editor::OpContext context{Context(), m_undo};
        return m_registry.Invoke("project.settings.configure", context, args);
    }
    editor::OpData PhysicsPatch(int hz)
    {
        auto patch = editor::OpData::MakeObject();
        auto physics = editor::OpData::MakeObject();
        physics.Set("hz", editor::OpData(hz)); patch.Set("physics", physics);
        return patch;
    }
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    int m_targetFps = 0;
    std::vector<input::InputAction> m_actions;
    std::vector<input::InputAxis> m_axes;
    bool m_inputEnabled = false;
};

TEST_F(ProjectSettingsOperatorTest, PartialEditKeepsOtherSettingsAndSupportsUndoRedo)
{
    const std::string before = Context().projectSettings.ToToml();

    const auto result = Configure(PhysicsPatch(120));

    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_EQ(Context().projectSettings.physics.hz, 120);
    EXPECT_EQ(Context().projectSettings.physics.substeps, 1);
    ASSERT_TRUE(m_undo.Undo());
    EXPECT_EQ(Context().projectSettings.ToToml(), before);
    ASSERT_TRUE(m_undo.Redo());
    EXPECT_EQ(Context().projectSettings.physics.hz, 120);
}

TEST_F(ProjectSettingsOperatorTest, UnknownNestedKeyRejectsTheWholePatch)
{
    const std::string before = Context().projectSettings.ToToml();
    auto patch = PhysicsPatch(120);
    auto invalid = editor::OpData::MakeObject();
    invalid.Set("doesNotExist", editor::OpData(true)); patch.Set("render", invalid);

    const auto result = Configure(patch);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(Context().projectSettings.ToToml(), before);
    EXPECT_FALSE(m_undo.CanUndo());
}

TEST_F(ProjectSettingsOperatorTest, ClampedNumbersAreRejectedInsteadOfReportingAChangedValue)
{
    const std::string before = Context().projectSettings.ToToml();

    const auto result = Configure(PhysicsPatch(1001));

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(Context().projectSettings.ToToml(), before);
    EXPECT_FALSE(m_undo.CanUndo());
}

TEST_F(ProjectSettingsOperatorTest, WrongScalarTypeIsRejectedWithoutChanges)
{
    const std::string before = Context().projectSettings.ToToml();
    auto patch = editor::OpData::MakeObject();
    auto physics = editor::OpData::MakeObject(); physics.Set("hz", editor::OpData("120"));
    patch.Set("physics", physics);

    const auto result = Configure(patch);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(Context().projectSettings.ToToml(), before);
}

TEST_F(ProjectSettingsOperatorTest, NewlyAuthoredCollisionPairsAreValidatedAndSaved)
{
    auto patch = editor::OpData::MakeObject();
    auto physics = editor::OpData::MakeObject();
    auto pairs = editor::OpData::MakeArray(); auto pair = editor::OpData::MakeArray();
    pair.Push(editor::OpData(1)); pair.Push(editor::OpData(2)); pairs.Push(pair);
    physics.Set("ignoreCollisions", pairs); patch.Set("physics", physics);

    const auto result = Configure(patch);

    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_FALSE(Context().projectSettings.physics.collisionMatrix.CanCollide(1, 2));
    EXPECT_FALSE(Context().projectSettings.physics.collisionMatrix.CanCollide(2, 1));
    EXPECT_TRUE(Context().projectSettings.physics.collisionMatrix.CanCollide(1, 3));
}

TEST_F(ProjectSettingsOperatorTest, UndoConflictKeepsSubsequentSettingsAndTheHistoryCursor)
{
    ASSERT_TRUE(Configure(PhysicsPatch(120)).ok);
    Context().projectSettings.physics.hz = 180;

    EXPECT_FALSE(m_undo.Undo());

    EXPECT_EQ(Context().projectSettings.physics.hz, 180);
    EXPECT_EQ(m_undo.GetCursor(), 1u);
}

TEST_F(ProjectSettingsOperatorTest, ParsingSettingsValuesDoesNotReloadOrClearInputBindings)
{
    input::InputAction sentinel; sentinel.name = "McpSettingsIsolation";
    sentinel.bindings.push_back({input::BindingSource::KEY, 74});
    ASSERT_TRUE(input::InputActionMap::AddAction(sentinel));
    const auto actions = input::InputActionMap::GetActions();
    const auto axes = input::InputActionMap::GetAxes();

    ASSERT_TRUE(Configure(PhysicsPatch(120)).ok);

    const auto& currentActions = input::InputActionMap::GetActions();
    const auto& currentAxes = input::InputActionMap::GetAxes();
    ASSERT_EQ(currentActions.size(), actions.size());
    ASSERT_EQ(currentAxes.size(), axes.size());
    for (std::size_t i = 0; i < actions.size(); ++i) {
        EXPECT_EQ(currentActions[i].name, actions[i].name);
        EXPECT_EQ(currentActions[i].bindings.size(), actions[i].bindings.size());
    }
    for (std::size_t i = 0; i < axes.size(); ++i) EXPECT_EQ(currentAxes[i].name, axes[i].name);
}

} /// @note namespace fbzz::tests
