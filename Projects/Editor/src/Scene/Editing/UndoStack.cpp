/// @file    UndoStack.cpp
/// @brief   コマンドの成否を確認し、競合時は履歴の位置を維持する。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include <Editor/Scene/Editing/UndoStack.hpp>
#include <Engine/Core/Diagnostics/Logger.hpp>
#include <algorithm>
#include <cassert>

namespace fbzz::editor {
void CheckedLambdaCommand::Execute()
{
    std::string reason;
    if (!TryExecute(reason)) {
        FBZZ_LOG_WARN("Command failed: %s", reason.c_str());
    }
}

void CheckedLambdaCommand::Undo()
{
    std::string reason;
    if (!TryUndo(reason)) {
        FBZZ_LOG_WARN("Undo failed: %s", reason.c_str());
    }
}
void CompositeCommand::Add(std::unique_ptr<ICommand> command) {
    if (command) {
        m_commands.push_back(std::move(command));
    }
}
bool CompositeCommand::SupportsLegacyComposition() const
{ return std::all_of(m_commands.begin(), m_commands.end(), [](const auto& command) { return command->SupportsLegacyComposition(); }); }
bool CompositeCommand::ValidateHistory(std::string& reason) const
{
    if (!SupportsLegacyComposition()) {
        reason = "Checked transactions cannot be placed in a legacy composite; use one atomic batch.";
        return false;
    }
    reason.clear();
    return true;
}
bool CompositeCommand::TryExecute(std::string& reason)
{
    if (!ValidateHistory(reason)) {
        return false;
    }
    for (const auto& command : m_commands) {
        command->Execute();
    }
    return true;
}
bool CompositeCommand::TryUndo(std::string& reason)
{
    if (!ValidateHistory(reason)) {
        return false;
    }
    for (auto item = m_commands.rbegin(); item != m_commands.rend(); ++item) {
        (*item)->Undo();
    }
    return true;
}
void CompositeCommand::Execute()
{
    std::string reason;
    if (!TryExecute(reason)) {
        FBZZ_LOG_WARN("Composite rejected: %s", reason.c_str());
    }
}

void CompositeCommand::Undo()
{
    std::string reason;
    if (!TryUndo(reason)) {
        FBZZ_LOG_WARN("Composite Undo rejected: %s", reason.c_str());
    }
}
void UndoStack::SetRecordingEnabled(bool enabled) { m_recordingEnabled = enabled; }
bool UndoStack::Failed()
{
    if (m_lastDiagnostic.empty()) {
        m_lastDiagnostic = "The command was rejected; no history transition was recorded.";
    }
    FBZZ_LOG_WARN("Editor history: %s", m_lastDiagnostic.c_str());
    return false;
}
bool UndoStack::Push(std::unique_ptr<ICommand> command)
{
    assert(command && "UndoStack::Push requires a valid command");
    if (!command) {
        return false;
    }
    if (m_isApplyingCommand || !m_recordingEnabled) {
        return false;
    }
    if (!command->ValidateHistory(m_lastDiagnostic)) {
        return Failed();
    }

    // 過去の位置で新しい操作を確定すると、Redo 側に残っていた履歴の枝は使えなくなる。
    if (m_cursor < m_history.size()) {
        m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(m_cursor), m_history.end());
    }
    m_history.push_back(std::move(command));
    if (m_history.size() > MAX_HISTORY) {
        m_history.erase(m_history.begin());
    }

    // 新規コマンドを適用済みとして扱い、履歴を参照する UI に変更を通知する。
    m_cursor = m_history.size();
    ++m_revision;
    m_lastDiagnostic.clear();
    return true;
}
bool UndoStack::Execute(std::unique_ptr<ICommand> command)
{
    assert(command && "UndoStack::Execute requires a valid command");
    if (!command) {
        return false;
    }
    if (m_isApplyingCommand) {
        return false;
    }
    if (!command->ValidateHistory(m_lastDiagnostic)) {
        return Failed();
    }
    // コマンド側のコールバックが履歴を触れても、実行中の履歴を移動させない。
    m_lastDiagnostic.clear();
    m_isApplyingCommand = true;
    const bool succeeded = command->TryExecute(m_lastDiagnostic);
    m_isApplyingCommand = false;
    if (!succeeded) {
        return Failed();
    }
    m_lastDiagnostic.clear();
    return m_recordingEnabled ? Push(std::move(command)) : true;
}
bool UndoStack::Undo()
{
    if (!CanUndo()) {
        return false;
    }
    m_lastDiagnostic.clear();
    m_isApplyingCommand = true;
    const bool succeeded = m_history[m_cursor - 1]->TryUndo(m_lastDiagnostic);
    m_isApplyingCommand = false;
    if (!succeeded) {
        return Failed();
    }

    // 操作が成功してから履歴の位置を戻す。失敗した場合は同じコマンドを再試行できる。
    --m_cursor;
    ++m_revision;
    m_lastDiagnostic.clear();
    return true;
}
bool UndoStack::Redo()
{
    if (!CanRedo()) {
        return false;
    }
    m_lastDiagnostic.clear();
    m_isApplyingCommand = true;
    const bool succeeded = m_history[m_cursor]->TryExecute(m_lastDiagnostic);
    m_isApplyingCommand = false;
    if (!succeeded) {
        return Failed();
    }

    // 再適用が成功した項目だけを履歴の適用済み側へ進める。
    ++m_cursor;
    ++m_revision;
    m_lastDiagnostic.clear();
    return true;
}
bool UndoStack::CanUndo() const { return !m_isApplyingCommand && m_recordingEnabled && m_cursor > 0 && m_cursor <= m_history.size(); }
bool UndoStack::CanRedo() const { return !m_isApplyingCommand && m_recordingEnabled && m_cursor < m_history.size(); }
void UndoStack::Clear()
{
    if (m_isApplyingCommand) {
        return;
    }
    m_history.clear();
    m_cursor = 0;
    ++m_revision;
    m_lastDiagnostic.clear();
}
std::string UndoStack::GetUndoDescription() const { return CanUndo() ? m_history[m_cursor - 1]->GetDescription() : std::string{}; }
std::string UndoStack::GetRedoDescription() const { return CanRedo() ? m_history[m_cursor]->GetDescription() : std::string{}; }
void UndoStack::JumpTo(std::size_t index)
{
    index = std::min(index, m_history.size());
    while (m_cursor > index) {
        if (!Undo()) {
            break;
        }
    }
    while (m_cursor < index) {
        if (!Redo()) {
            break;
        }
    }
}
std::vector<UndoStack::HistoryEntry> UndoStack::GetHistory() const
{
    std::vector<HistoryEntry> result;
    result.reserve(m_history.size());
    for (std::size_t index = 0; index < m_history.size(); ++index) {
        result.push_back({m_history[index]->GetDescription(), index < m_cursor});
    }
    return result;
}
} // namespace fbzz::editor
