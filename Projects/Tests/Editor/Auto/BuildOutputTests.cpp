/// @file    BuildOutputTests.cpp
/// @brief   履歴を選択したときに同じビルドの診断とログを参照できることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-17
#include <TestKit/Editor/EditorFixture.hpp>
#include <TestKit/TestKit.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Panels/BuildOutputPanel.hpp>
#include <Editor/Util/UndoStack.hpp>

namespace fbzz::tests {
class BuildOutputTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        Context().buildConsole = &m_console;
        m_panel.OnRegisterOperators(m_registry);
    }

    editor::OpResult Invoke(const char* name, const editor::OpArgs& args = {})
    {
        editor::OpContext context{ Context(), m_undo };
        return m_registry.Invoke(name, context, args);
    }

    editor::BuildConsole m_console;
    editor::BuildOutputPanel m_panel;
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
};

/// @brief 過去のレコードを選ぶと、その時の生ログを返す。最新ログの問い合わせも維持する。
TEST_F(BuildOutputTest, PastSelectionHasItsOwnLog)
{
    m_console.BeginBuild(editor::BuildRecord::Kind::Hlsl);
    m_console.IngestFullLog("first build output\n");
    m_console.EndBuild(false, 1);
    m_console.BeginBuild(editor::BuildRecord::Kind::Hlsl);
    m_console.IngestFullLog("second build output\n");

    editor::OpArgs select;
    select.Set("action", std::string("select"));
    select.Set("index", 0);
    ASSERT_TRUE(Invoke("build.output.control", select).ok);
    const auto output = Invoke("build.output.inspect");

    ASSERT_TRUE(output.ok);
    const auto* selected = output.data.Find("selectedLog");
    ASSERT_NE(selected, nullptr);
    EXPECT_EQ(selected->AsString(), "first build output\n");
    EXPECT_EQ(output.data.Find("liveLog")->AsString(), "second build output\n");
}
} /// @note namespace fbzz::tests
