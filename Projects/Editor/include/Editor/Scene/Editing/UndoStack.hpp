/// @file    UndoStack.hpp
/// @brief   Commands with checked Undo/Redo and a linear editor history.
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::editor {
class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void Execute() = 0;
    virtual void Undo() = 0;
    virtual std::string GetDescription() const = 0;
    /// @note Legacy commands are assumed to succeed; transactional commands override these before history moves.
    virtual bool TryExecute(std::string& reason) { reason.clear(); Execute(); return true; }
    virtual bool TryUndo(std::string& reason) { reason.clear(); Undo(); return true; }
    virtual bool SupportsLegacyComposition() const { return true; }
    virtual bool ValidateHistory(std::string& reason) const { reason.clear(); return true; }
};

class LambdaCommand final : public ICommand {
public:
    using Action = std::function<void()>;
    LambdaCommand(std::string description, Action execute, Action undo)
        : m_description(std::move(description)), m_execute(std::move(execute)), m_undo(std::move(undo)) {}
    void Execute() override { if (m_execute) m_execute(); }
    void Undo() override { if (m_undo) m_undo(); }
    std::string GetDescription() const override { return m_description; }
private:
    std::string m_description;
    Action m_execute;
    Action m_undo;
};

/// @note One checked command owns an entire atomic transaction; it cannot be wrapped in legacy composition.
class CheckedLambdaCommand final : public ICommand {
public:
    using Action = std::function<bool(std::string&)>;
    CheckedLambdaCommand(std::string description, Action execute, Action undo)
        : m_description(std::move(description)), m_execute(std::move(execute)), m_undo(std::move(undo)) {}
    void Execute() override;
    void Undo() override;
    bool TryExecute(std::string& reason) override { return m_execute ? m_execute(reason) : true; }
    bool TryUndo(std::string& reason) override { return m_undo ? m_undo(reason) : true; }
    bool SupportsLegacyComposition() const override { return false; }
    std::string GetDescription() const override { return m_description; }
private:
    std::string m_description;
    Action m_execute;
    Action m_undo;
};

/// @note This legacy grouping requires every child to support unconditional execution and Undo; checked batches remain standalone.
class CompositeCommand final : public ICommand {
public:
    explicit CompositeCommand(std::string description) : m_description(std::move(description)) {}
    void Add(std::unique_ptr<ICommand> command);
    bool Empty() const { return m_commands.empty(); }
    void Execute() override;
    void Undo() override;
    bool TryExecute(std::string& reason) override;
    bool TryUndo(std::string& reason) override;
    bool SupportsLegacyComposition() const override;
    bool ValidateHistory(std::string& reason) const override;
    std::string GetDescription() const override { return m_description; }
private:
    std::string m_description;
    std::vector<std::unique_ptr<ICommand>> m_commands;
};

class UndoStack {
public:
    static constexpr std::size_t MAX_HISTORY = 128;
    void SetRecordingEnabled(bool enabled);
    [[nodiscard]] bool IsRecordingEnabled() const { return m_recordingEnabled; }
    /// @note Push records an already applied command; it never executes it again.
    bool Push(std::unique_ptr<ICommand> command);
    bool Execute(std::unique_ptr<ICommand> command);
    /// @note Failure preserves history cursor/revision and exposes its reason through LastDiagnostic.
    bool Undo();
    bool Redo();
    [[nodiscard]] bool CanUndo() const;
    [[nodiscard]] bool CanRedo() const;
    void Clear();
    [[nodiscard]] const std::string& LastDiagnostic() const { return m_lastDiagnostic; }
    [[nodiscard]] std::string GetUndoDescription() const;
    [[nodiscard]] std::string GetRedoDescription() const;
    [[nodiscard]] std::size_t GetHistorySize() const { return m_history.size(); }
    [[nodiscard]] std::size_t GetCursor() const { return m_cursor; }
    [[nodiscard]] std::size_t GetRevision() const { return m_revision; }
    /// @note Stops at the first failed transition rather than skipping a conflicted command.
    void JumpTo(std::size_t index);
    struct HistoryEntry { std::string description; bool applied; };
    [[nodiscard]] std::vector<HistoryEntry> GetHistory() const;
private:
    bool Failed();
    std::vector<std::unique_ptr<ICommand>> m_history;
    std::size_t m_cursor = 0;
    bool m_isApplyingCommand = false;
    bool m_recordingEnabled = true;
    std::size_t m_revision = 0;
    std::string m_lastDiagnostic;
};
} /// @note namespace fbzz::editor