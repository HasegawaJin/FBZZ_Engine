/// @file    UndoHistoryPanel.hpp
/// @brief   Undo 履歴を一覧表示し、クリックで任意のステートへジャンプするパネル。
/// @author  Hasegawa Jin
/// @date    2026-06-16
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

    std::size_t m_lastRevision = static_cast<std::size_t>(-1); ///< 履歴一覧を最後に表示した時点の UndoStack リビジョン。差分があれば表示位置を更新する。
};

} // namespace fbzz::editor
