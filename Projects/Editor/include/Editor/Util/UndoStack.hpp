// FBZZ Engine
// UndoStack.hpp | fbzz::editor
// コマンドパターンによる Undo/Redo スタック
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace fbzz::editor {

class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void Execute() = 0;
    virtual void Undo()    = 0;
    virtual std::string GetDescription() const = 0;
};

class UndoStack {
public:
    static constexpr int MAX_HISTORY = 64;

    // cmd は Execute() 済みの状態で渡す
    void Push(std::unique_ptr<ICommand> cmd);
    void Undo();
    void Redo();
    bool CanUndo() const;
    bool CanRedo() const;
    void Clear();

    std::string GetUndoDescription() const;
    std::string GetRedoDescription() const;

private:
    std::vector<std::unique_ptr<ICommand>> m_history;
    int m_cursor = -1; // -1 = 履歴なし。0 以上は m_history 内の現在位置を指す
};

} // namespace fbzz::editor
