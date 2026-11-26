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
    HotkeyScope GetHotkeyScope() const override { return HotkeyScope::HotkeyEditor; }
    bool IsCapturingKeyboard() const { return visible && !m_rebindTarget.empty(); }
    void OnRegisterOperators(OperatorRegistry& registry) override;

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    char m_search[128]{}; ///< 登録済みホットキーを名前で絞り込む入力欄。
    std::string m_rebindTarget; ///< 空のときは次のキーを待ち受けない。
    /// @brief 直前のリバインドで既存の割り当てとぶつかった場合の警告文 (空 = 警告なし)。
    /// @note 競合の理由を表示して別のキーを待つ。
    std::string m_conflictNote; ///< 入力したキーの組み合わせが既存操作と競合する場合の診断。
};

} // namespace fbzz::editor
