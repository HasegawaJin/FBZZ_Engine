// FBZZ Engine
// ModalDialog.hpp | fbzz::editor
// ImGui モーダル確認ダイアログの汎用ラッパー
#pragma once
#include <functional>
#include <string>

namespace fbzz::editor {

class ModalDialog {
public:
    // 次フレームから表示するモーダルを予約する
    static void OpenConfirm(const std::string& title,
                            const std::string& message,
                            std::function<void()> onConfirm);
    static void OpenUnsavedChanges(const std::string& title,
                                   const std::string& message,
                                   std::function<bool()> onSave,
                                   std::function<void()> onDiscard);

    // 毎フレーム EditorApp から呼ぶ
    static void OnRender();

private:
    struct State {
        std::string           title;
        std::string           message;
        std::function<void()> onConfirm;
        std::function<bool()> onSave;
        std::function<void()> onDiscard;
        bool                  pending = false;
        bool                  opened  = false;
    };
    static State s_state;
};

} // namespace fbzz::editor
