/// @file    HotkeyEditorPanel.hpp
/// @brief   ホットキー一覧表示とリバインド UI。
/// @author  Hasegawa Jin
/// @date    2026-06-16
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <string>

namespace fbzz::editor {

class HotkeyEditorPanel final : public IPanel {
public:
    const char* GetWindowName()        const override { return "Hotkey Editor"; }
    const char* GetViewMenuName()      const override { return "Hotkey Editor"; }
    bool        GetDefaultVisibility() const override { return false; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    /// リバインド待ち状態
    std::string m_rebindTarget;     ///< 待機中のホットキー名 (空 = 待機していない)
    /// 直前のリバインドで既存の割り当てとぶつかった場合の警告文 (空 = 警告なし)。
    /// @note 割り当ては通したうえで知らせる。拒否すると理由が分からず、黙って通すと後から気づく。
    std::string m_conflictNote;
};

} // namespace fbzz::editor
