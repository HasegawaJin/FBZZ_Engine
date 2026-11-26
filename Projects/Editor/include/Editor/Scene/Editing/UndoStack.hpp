/// @file    UndoStack.hpp
/// @brief   実行と取り消しの成否を確認できる Editor コマンドと線形履歴。
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
    /// @note 既定の実装は成功として扱う。失敗しうるコマンドは履歴が移動する前に成否を返すために上書きする。
    /// @param[out] reason 実行に失敗した理由。既定の実装では空にする。
    virtual bool TryExecute(std::string& reason)
    {
        reason.clear();
        Execute();
        return true;
    }
    /// @param[out] reason 取り消しに失敗した理由。既定の実装では空にする。
    virtual bool TryUndo(std::string& reason)
    {
        reason.clear();
        Undo();
        return true;
    }
    virtual bool SupportsLegacyComposition() const { return true; }
    virtual bool ValidateHistory(std::string& reason) const { reason.clear(); return true; }
};

class LambdaCommand final : public ICommand {
public:
    using Action = std::function<void()>;
    LambdaCommand(std::string description, Action execute, Action undo)
        : m_description(std::move(description)), m_execute(std::move(execute)), m_undo(std::move(undo)) {}
    void Execute() override {
        if (m_execute) {
            m_execute();
        }
    }
    void Undo() override {
        if (m_undo) {
            m_undo();
        }
    }
    std::string GetDescription() const override { return m_description; }
private:
    std::string m_description; ///< 履歴に表示する操作名。
    Action m_execute; ///< 操作を適用する処理。
    Action m_undo; ///< 適用した操作を取り消す処理。
};

/// @brief 処理の成否を返し、一つのトランザクション全体を履歴へ登録するコマンド。
/// @note 成否を扱わない CompositeCommand には登録できない。
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
    std::string m_description; ///< 履歴に表示するトランザクション名。
    Action m_execute; ///< 操作全体を適用し、失敗した場合はその理由を返す処理。
    Action m_undo; ///< 操作全体を取り消し、失敗した場合はその理由を返す処理。
};

/// @brief 成否の確認が不要なコマンド群を一つの履歴項目として扱う。
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
    std::string m_description; ///< 履歴に表示する一連の操作名。
    std::vector<std::unique_ptr<ICommand>> m_commands; ///< まとめたコマンド。格納順に実行し、逆順に取り消す。
};

class UndoStack {
public:
    static constexpr std::size_t MAX_HISTORY = 128;
    void SetRecordingEnabled(bool enabled);
    [[nodiscard]] bool IsRecordingEnabled() const { return m_recordingEnabled; }
    /// @note すでに適用済みのコマンドを履歴へ登録する。
    bool Push(std::unique_ptr<ICommand> command);
    bool Execute(std::unique_ptr<ICommand> command);
    /// @note 失敗時は履歴上の位置とリビジョンを維持し、理由を LastDiagnostic() で取得できる。
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
    /// @note 移動中にコマンドが失敗したら、その位置で止まる。
    void JumpTo(std::size_t index);
    struct HistoryEntry {
        std::string description; ///< 履歴へ表示するコマンド名。
        bool applied; ///< コマンドが現在の履歴位置より前にあり、適用済みであるか。
    };
    [[nodiscard]] std::vector<HistoryEntry> GetHistory() const;
private:
    bool Failed();
    std::vector<std::unique_ptr<ICommand>> m_history; ///< 古い順に並ぶ登録済みコマンド。
    std::size_t m_cursor = 0; ///< 適用済みコマンド数。次の Redo はこの添字から始める。
    bool m_isApplyingCommand = false; ///< コマンド呼び出し中に履歴へ再入するのを防ぐ。
    bool m_recordingEnabled = true; ///< 新規コマンドの記録と Undo / Redo を許可するか。
    std::size_t m_revision = 0; ///< 履歴の内容かカーソルが変わるたびに進む番号。
    std::string m_lastDiagnostic; ///< 直近の履歴操作でコマンドが失敗した理由。
};
} // namespace fbzz::editor
