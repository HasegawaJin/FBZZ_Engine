/// @file    UndoStackTests.cpp
/// @brief   Undo/Redo の線形履歴と、記録停止・履歴上限・複合操作の順序を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// 編集の取り消しは «壊れても壊れたと分からない» 種類の機能で、気づくのは
/// 「Undo したら別のものが消えた」という取り返しのつかない場面になる。
/// 順序と境界をここで固定する。
#include <TestKit/TestKit.hpp>

#include <Editor/Util/UndoStack.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

using editor::CompositeCommand;
using editor::ICommand;
using editor::LambdaCommand;
using editor::UndoStack;

/// 実行と取り消しの «順序» を記録する。値の一致だけ見ると順序の誤りを見逃す。
std::unique_ptr<ICommand> Trace(std::vector<std::string>& log, const std::string& name)
{
    return std::make_unique<LambdaCommand>(
        name,
        [&log, name] { log.push_back("do:" + name); },
        [&log, name] { log.push_back("undo:" + name); });
}

std::unique_ptr<ICommand> Assign(int& target, int value, int previous,
                                 const std::string& name = "assign")
{
    return std::make_unique<LambdaCommand>(
        name,
        [&target, value] { target = value; },
        [&target, previous] { target = previous; });
}

} // namespace

TEST(UndoStack, StartsEmpty)
{
    UndoStack stack;
    EXPECT_FALSE(stack.CanUndo());
    EXPECT_FALSE(stack.CanRedo());
    EXPECT_EQ(stack.GetHistorySize(), 0u);
    EXPECT_EQ(stack.GetUndoDescription(), "");
    EXPECT_EQ(stack.GetRedoDescription(), "");
}

TEST(UndoStack, ExecuteRunsTheCommandAndRecordsIt)
{
    UndoStack stack;
    int value = 0;
    stack.Execute(Assign(value, 5, 0));

    EXPECT_EQ(value, 5);
    EXPECT_TRUE(stack.CanUndo());
    EXPECT_EQ(stack.GetHistorySize(), 1u);
}

TEST(UndoStack, PushRecordsWithoutRunning)
{
    /// @note Push は «すでに適用済み» の操作を履歴へ載せる入口。ここで実行してしまうと二重適用になる。
    UndoStack stack;
    int value = 0;
    stack.Push(Assign(value, 5, 0));

    EXPECT_EQ(value, 0);
    EXPECT_TRUE(stack.CanUndo());
}

TEST(UndoStack, UndoAndRedoWalkTheHistory)
{
    UndoStack stack;
    int value = 0;
    stack.Execute(Assign(value, 1, 0));
    stack.Execute(Assign(value, 2, 1));
    ASSERT_EQ(value, 2);

    stack.Undo();
    EXPECT_EQ(value, 1);
    stack.Undo();
    EXPECT_EQ(value, 0);
    EXPECT_FALSE(stack.CanUndo());

    stack.Redo();
    EXPECT_EQ(value, 1);
    stack.Redo();
    EXPECT_EQ(value, 2);
    EXPECT_FALSE(stack.CanRedo());
}

TEST(UndoStack, ExtraUndoOrRedoIsIgnored)
{
    UndoStack stack;
    int value = 0;
    stack.Execute(Assign(value, 1, 0));

    stack.Undo();
    /// @note もう戻れない
    stack.Undo();
    EXPECT_EQ(value, 0);

    stack.Redo();
    /// @note もう進めない
    stack.Redo();
    EXPECT_EQ(value, 1);
}

TEST(UndoStack, NewWorkAfterUndoDiscardsTheRedoBranch)
{
    /// @note 線形履歴。戻ってから別の操作をしたら、やり直せた側は消える。
    UndoStack stack;
    std::vector<std::string> log;
    stack.Execute(Trace(log, "a"));
    stack.Execute(Trace(log, "b"));
    stack.Undo();
    ASSERT_TRUE(stack.CanRedo());

    stack.Execute(Trace(log, "c"));

    EXPECT_FALSE(stack.CanRedo());
    EXPECT_EQ(stack.GetHistorySize(), 2u);
    EXPECT_EQ(stack.GetUndoDescription(), "c");
}

TEST(UndoStack, DescriptionsFollowTheCursor)
{
    UndoStack stack;
    int value = 0;
    stack.Execute(Assign(value, 1, 0, "first"));
    stack.Execute(Assign(value, 2, 1, "second"));

    EXPECT_EQ(stack.GetUndoDescription(), "second");
    EXPECT_EQ(stack.GetRedoDescription(), "");

    stack.Undo();
    EXPECT_EQ(stack.GetUndoDescription(), "first");
    EXPECT_EQ(stack.GetRedoDescription(), "second");
}

TEST(UndoStack, HistoryMarksWhichEntriesAreApplied)
{
    UndoStack stack;
    int value = 0;
    stack.Execute(Assign(value, 1, 0, "first"));
    stack.Execute(Assign(value, 2, 1, "second"));
    stack.Undo();

    const auto history = stack.GetHistory();
    ASSERT_EQ(history.size(), 2u);
    EXPECT_EQ(history[0].description, "first");
    EXPECT_TRUE(history[0].applied);
    EXPECT_FALSE(history[1].applied);
}

TEST(UndoStack, JumpToMovesInEitherDirection)
{
    UndoStack stack;
    int value = 0;
    stack.Execute(Assign(value, 1, 0));
    stack.Execute(Assign(value, 2, 1));
    stack.Execute(Assign(value, 3, 2));

    stack.JumpTo(1);
    EXPECT_EQ(value, 1);
    EXPECT_EQ(stack.GetCursor(), 1u);

    stack.JumpTo(3);
    EXPECT_EQ(value, 3);

    stack.JumpTo(0);
    EXPECT_EQ(value, 0);
}

TEST(UndoStack, JumpToClampsBeyondTheEnd)
{
    UndoStack stack;
    int value = 0;
    stack.Execute(Assign(value, 1, 0));

    stack.JumpTo(999);
    EXPECT_EQ(stack.GetCursor(), stack.GetHistorySize());
    EXPECT_EQ(value, 1);
}

TEST(UndoStack, ClearDropsEverything)
{
    UndoStack stack;
    int value = 0;
    stack.Execute(Assign(value, 1, 0));
    stack.Clear();

    EXPECT_EQ(stack.GetHistorySize(), 0u);
    EXPECT_FALSE(stack.CanUndo());
    EXPECT_FALSE(stack.CanRedo());
    /// @note 履歴を捨てるだけで、状態は戻さない
    EXPECT_EQ(value, 1);
}

TEST(UndoStack, RecordingDisabledStillAppliesButKeepsNoHistory)
{
    /// @note Play 中の編集はランタイム側の変更なので、履歴に残すと «停止後に戻せてしまう»。
    UndoStack stack;
    int value = 0;
    stack.SetRecordingEnabled(false);
    stack.Execute(Assign(value, 5, 0));

    EXPECT_EQ(value, 5);
    EXPECT_EQ(stack.GetHistorySize(), 0u);
    EXPECT_FALSE(stack.CanUndo());
}

TEST(UndoStack, RecordingDisabledSuppressesUndoOfEarlierWork)
{
    UndoStack stack;
    int value = 0;
    stack.Execute(Assign(value, 1, 0));
    stack.SetRecordingEnabled(false);

    EXPECT_FALSE(stack.CanUndo());
    stack.Undo();
    EXPECT_EQ(value, 1);

    stack.SetRecordingEnabled(true);
    EXPECT_TRUE(stack.CanUndo());
}

TEST(UndoStack, RevisionAdvancesOnRecordedWork)
{
    /// @note Revision は «保存が要るか» の判定に使われる。記録が増えたら必ず動く必要がある。
    UndoStack stack;
    int value = 0;
    const std::size_t before = stack.GetRevision();

    stack.Execute(Assign(value, 1, 0));
    EXPECT_NE(stack.GetRevision(), before);
}

TEST(UndoStack, DropsTheOldestEntryBeyondTheHistoryLimit)
{
    UndoStack stack;
    std::vector<std::string> log;
    for (std::size_t i = 0; i < UndoStack::MAX_HISTORY + 5; ++i)
        stack.Execute(Trace(log, "cmd" + std::to_string(i)));

    EXPECT_EQ(stack.GetHistorySize(), UndoStack::MAX_HISTORY);
    /// @note 最新側が残る。いちばん古い "cmd0" は押し出されている。
    EXPECT_EQ(stack.GetHistory().front().description, "cmd5");
    EXPECT_EQ(stack.GetUndoDescription(),
              "cmd" + std::to_string(UndoStack::MAX_HISTORY + 4));
}

TEST(UndoStack, CompositeUndoesInReverseOrder)
{
    /// @note 親作成 → 子作成 の複合なら、戻すときは 子 → 親 でなければ破棄が壊れる。
    std::vector<std::string> log;
    auto composite = std::make_unique<CompositeCommand>("group");
    composite->Add(Trace(log, "parent"));
    composite->Add(Trace(log, "child"));

    UndoStack stack;
    stack.Execute(std::move(composite));
    stack.Undo();

    const std::vector<std::string> expected{
        "do:parent", "do:child", "undo:child", "undo:parent"
    };
    EXPECT_EQ(log, expected);
}

TEST(UndoStack, CompositeIgnoresNullMembers)
{
    CompositeCommand composite("group");
    composite.Add(nullptr);
    EXPECT_TRUE(composite.Empty());
}

} // namespace fbzz::tests
