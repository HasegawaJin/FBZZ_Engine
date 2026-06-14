// FBZZ Engine
// UndoStack.cpp | fbzz::editor
// コマンドパターンによる Undo/Redo スタック実装
#include <Editor/Util/UndoStack.hpp>
#include <cassert>

namespace fbzz::editor {

void UndoStack::SetRecordingEnabled(bool enabled)
{
    m_recordingEnabled = enabled;
}

void CompositeCommand::Add(std::unique_ptr<ICommand> command)
{
    if (command)
        m_commands.push_back(std::move(command));
}

void CompositeCommand::Execute()
{
    for (const auto& command : m_commands)
        command->Execute();
}

void CompositeCommand::Undo()
{
    // 複合操作は適用と逆順で戻す。親作成→子作成なら子→親の順で破棄する必要がある。
    for (auto it = m_commands.rbegin(); it != m_commands.rend(); ++it)
        (*it)->Undo();
}

void UndoStack::Push(std::unique_ptr<ICommand> cmd)
{
    assert(cmd && "UndoStack::Push requires a valid command");
    if (!cmd || m_isApplyingCommand || !m_recordingEnabled) return;

    // カーソルより後ろの履歴を破棄 (新しい操作でやり直し履歴は消える — 線形履歴)
    if (m_cursor < m_history.size())
        m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(m_cursor), m_history.end());

    m_history.push_back(std::move(cmd));

    // 最大履歴数を超えた場合は最古のコマンドを削除する。
    if (m_history.size() > MAX_HISTORY)
        m_history.erase(m_history.begin());

    m_cursor = m_history.size();
    ++m_revision;
}

void UndoStack::Execute(std::unique_ptr<ICommand> cmd)
{
    assert(cmd && "UndoStack::Execute requires a valid command");
    if (!cmd || m_isApplyingCommand) return;

    // Play/Pause 中もユーザー操作自体は適用するが、ランタイム編集は履歴へ残さない。
    if (!m_recordingEnabled) {
        m_isApplyingCommand = true;
        cmd->Execute();
        m_isApplyingCommand = false;
        return;
    }

    m_isApplyingCommand = true;
    cmd->Execute();
    m_isApplyingCommand = false;
    Push(std::move(cmd));
}

void UndoStack::Undo()
{
    if (!CanUndo()) return;

    m_isApplyingCommand = true;
    --m_cursor;
    m_history[m_cursor]->Undo();
    m_isApplyingCommand = false;
}

void UndoStack::Redo()
{
    if (!CanRedo()) return;

    m_isApplyingCommand = true;
    m_history[m_cursor]->Execute();
    ++m_cursor;
    m_isApplyingCommand = false;
}

bool UndoStack::CanUndo() const
{
    return m_recordingEnabled && m_cursor > 0 && m_cursor <= m_history.size();
}

bool UndoStack::CanRedo() const
{
    return m_recordingEnabled && m_cursor < m_history.size();
}

void UndoStack::Clear()
{
    if (m_isApplyingCommand) return;

    m_history.clear();
    m_cursor = 0;
    ++m_revision;
}

std::string UndoStack::GetUndoDescription() const
{
    if (!CanUndo()) return "";
    return m_history[m_cursor - 1]->GetDescription();
}

std::string UndoStack::GetRedoDescription() const
{
    if (!CanRedo()) return "";
    return m_history[m_cursor]->GetDescription();
}

} // namespace fbzz::editor
