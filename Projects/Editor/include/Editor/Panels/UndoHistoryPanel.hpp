// FBZZ Engine
// UndoHistoryPanel.hpp | fbzz::editor
// Undo 履歴を一覧表示し、クリックで任意のステートへジャンプするパネル
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <cstddef>

namespace fbzz::editor {

class UndoHistoryPanel : public IPanel {
public:
    const char* GetWindowName()        const override { return "Undo History"; }
    const char* GetViewMenuName()      const override { return "Undo History"; }
    bool        GetDefaultVisibility() const override { return false; }

private:
    void OnRenderContent(EditorContext& ctx) override;

    std::size_t m_lastRevision = static_cast<std::size_t>(-1);
};

} // namespace fbzz::editor
