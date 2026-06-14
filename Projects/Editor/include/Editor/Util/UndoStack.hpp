// FBZZ Engine
// UndoStack.hpp | fbzz::editor
// コマンドパターンによる Undo/Redo スタック
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

    // WHAT: コマンドが表す編集を適用する。
    // WHY: 初回実行と Redo で同じ処理を共有し、再実行時の挙動差を防ぐ。
    virtual void Execute() = 0;

    // WHAT: Execute() 前の状態へ編集対象を戻す。
    virtual void Undo()    = 0;

    // WHAT: Edit メニューに表示するユーザー向け操作名を返す。
    virtual std::string GetDescription() const = 0;
};

// 任意の Editor 操作を Undo/Redo 可能にする汎用コマンド。
// WHY: 各パネル専用の小さな ICommand 派生型を乱立させず、EntityID 等の
//      安定した識別子をラムダへ保持して対象を再解決できるようにする。
class LambdaCommand final : public ICommand {
public:
    using Action = std::function<void()>;

    LambdaCommand(std::string description, Action execute, Action undo)
        : m_description(std::move(description))
        , m_execute(std::move(execute))
        , m_undo(std::move(undo))
    {
    }

    void Execute() override { if (m_execute) m_execute(); }
    void Undo() override { if (m_undo) m_undo(); }
    std::string GetDescription() const override { return m_description; }

private:
    std::string m_description;
    Action      m_execute;
    Action      m_undo;
};

// 複数の編集をユーザー視点の1操作として扱うコマンド。
// WHY: GameObject 作成時の子生成や複数選択編集を1回の Undo で戻すため。
class CompositeCommand final : public ICommand {
public:
    explicit CompositeCommand(std::string description)
        : m_description(std::move(description))
    {
    }

    void Add(std::unique_ptr<ICommand> command);
    bool Empty() const { return m_commands.empty(); }

    void Execute() override;
    void Undo() override;
    std::string GetDescription() const override { return m_description; }

private:
    std::string                            m_description;
    std::vector<std::unique_ptr<ICommand>> m_commands;
};

// Editor 全体で共有する線形 Undo/Redo 履歴。
// WHY: パネルごとに履歴を持つと操作順序が崩れるため、EditorApp が1つだけ所有する。
class UndoStack {
public:
    static constexpr std::size_t MAX_HISTORY = 128;

    // Play/Pause 中はランタイム状態を履歴へ混入させないため、EditorApp から記録を停止する。
    void SetRecordingEnabled(bool enabled);
    [[nodiscard]] bool IsRecordingEnabled() const { return m_recordingEnabled; }

    // WHAT: 既に画面へ反映済みのコマンドを履歴へ追加する。
    // WHY: ImGui のドラッグ編集は UI が値を直接更新するため、確定時には再実行しない。
    void Push(std::unique_ptr<ICommand> cmd);

    // WHAT: コマンドを実行してから履歴へ追加する。
    // WHY: メニュー操作等を Execute と Push の書き忘れなく記録する。
    void Execute(std::unique_ptr<ICommand> cmd);

    void Undo();
    void Redo();
    [[nodiscard]] bool CanUndo() const;
    [[nodiscard]] bool CanRedo() const;
    void Clear();

    [[nodiscard]] std::string GetUndoDescription() const;
    [[nodiscard]] std::string GetRedoDescription() const;
    [[nodiscard]] std::size_t GetHistorySize() const { return m_history.size(); }
    [[nodiscard]] std::size_t GetRevision() const { return m_revision; }

private:
    std::vector<std::unique_ptr<ICommand>> m_history;

    // m_cursor は「適用済みコマンド数」を表す。
    // WHY: -1 を番兵にする添字方式では Undo 後の erase 境界が符号付き演算になり、
    //      vector の size_type と混在するため、常に [0, size] の範囲で管理する。
    std::size_t m_cursor = 0;

    // Undo/Redo コールバックから履歴を変更すると実行中コマンドが破棄され得るため、
    // コマンド適用中の Push/Execute を拒否して vector の再入変更を防ぐ。
    bool m_isApplyingCommand = false;
    bool m_recordingEnabled = true;

    // 履歴件数が同じでも Redo 破棄後の Push は別状態なので、変更検知には世代を使う。
    std::size_t m_revision = 0;
};

} // namespace fbzz::editor
