/// @file    ModalDialog.hpp
/// @brief   ImGui モーダル確認ダイアログの汎用ラッパー。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <functional>
#include <string>

namespace fbzz::editor {

class ModalDialog {
public:
    /// 次フレームから表示するモーダルを予約する
    static void OpenConfirm(const std::string& title,
                            const std::string& message,
                            std::function<void()> onConfirm);
    static void OpenUnsavedChanges(const std::string& title,
                                   const std::string& message,
                                   std::function<bool()> onSave,
                                   std::function<void()> onDiscard);

    /// テキスト入力モーダル。Confirm 時に入力文字列を渡す。
    /// hint: InputText のデフォルト値。note: フィールド下に薄く表示する補足テキスト (省略可)
    static void OpenInput(const std::string& title,
                          const std::string& hint,
                          std::function<void(const std::string& input)> onConfirm,
                          const std::string& note = {});

    /// 毎フレーム EditorApp から呼ぶ
    static void OnRender();

private:
    struct State {
        std::string           title;
        std::string           message;
        /// @note OpenInput 用補足テキスト
        std::string           note;
        std::function<void()> onConfirm;
        std::function<bool()> onSave;
        std::function<void()> onDiscard;
        /// @note OpenInput 用コールバック
        std::function<void(const std::string&)> onInput;
        char                  inputBuf[256] = {};
        bool                  isInput       = false;
        /// @note 初回フォーカスを1度だけ当てる
        bool                  inputNeedsFocus = false;
        bool                  pending  = false;
        bool                  opened   = false;
    };
    static State s_state;
};

} // namespace fbzz::editor
