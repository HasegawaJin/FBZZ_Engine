// FBZZ Engine
// HotkeyEditorPanel.hpp | fbzz::editor
// ホットキー一覧表示とリバインド UI
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <string>

namespace fbzz::editor {

class HotkeyEditorPanel final : public IPanel {
public:
    const char* GetWindowName()   const override { return "Hotkey Editor"; }
    const char* GetViewMenuName() const override { return "Hotkey Editor"; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    // リバインド待ち状態
    std::string m_rebindTarget;     // 待機中のホットキー名 (空 = 待機していない)
};

} // namespace fbzz::editor
