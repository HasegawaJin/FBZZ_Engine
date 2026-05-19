// FBZZ Engine
// UndoStack.cpp | fbzz::editor
// コマンドパターンによる Undo/Redo スタック実装
#include <editor/Util/UndoStack.hpp>
#include <cassert>

namespace fbzz::editor {

void UndoStack::Push(std::unique_ptr<ICommand> cmd)
{
    // カーソルより後ろの履歴を破棄 (新しい操作でやり直し履歴は消える)
    if (m_cursor + 1 < static_cast<int>(m_history.size()))
        m_history.erase(m_history.begin() + m_cursor + 1, m_history.end());

    m_history.push_back(std::move(cmd));

    // 最大履歴数を超えた場合は先頭を削除
    if (static_cast<int>(m_history.size()) > MAX_HISTORY)
        m_history.erase(m_history.begin());

    m_cursor = static_cast<int>(m_history.size()) - 1;
}

void UndoStack::Undo()
{
    if (!CanUndo()) return;
    m_history[m_cursor]->Undo();
    --m_cursor;
}

void UndoStack::Redo()
{
    if (!CanRedo()) return;
    ++m_cursor;
    m_history[m_cursor]->Execute();
}

bool UndoStack::CanUndo() const { return m_cursor >= 0; }
bool UndoStack::CanRedo() const { return m_cursor + 1 < static_cast<int>(m_history.size()); }

void UndoStack::Clear()
{
    m_history.clear();
    m_cursor = -1;
}

std::string UndoStack::GetUndoDescription() const
{
    if (!CanUndo()) return "";
    return m_history[m_cursor]->GetDescription();
}

std::string UndoStack::GetRedoDescription() const
{
    if (!CanRedo()) return "";
    return m_history[m_cursor + 1]->GetDescription();
}

} // namespace fbzz::editor
