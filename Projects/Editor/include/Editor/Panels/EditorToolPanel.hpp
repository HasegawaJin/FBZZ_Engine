/// @file    EditorToolPanel.hpp
/// @brief   Effect/Map等の制作ツールが共通利用するEditorパネル基底クラス。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once

#include <Editor/Panels/IPanel.hpp>

namespace fbzz::editor {

/// 制作ツール用の薄い共通層。
/// @note 個別ツールが Window 登録・共通ヘッダー・カテゴリ分けを重複実装すると、
///       新しい Editor を追加するたびに UI 規約が分岐する。
class EditorToolPanel : public IPanel {
public:
    const char* GetViewMenuName() const override { return GetEditorType(); }
    const char* GetMenuCategory() const override { return "Editors"; }

protected:
    virtual const char* GetEditorType() const = 0;

    void DrawEditorToolbar(EditorContext& ctx, const char* documentName)
    {
        ImGui::Text("%s", documentName ? documentName : GetEditorType());
        ImGui::SameLine();
        if (ImGui::Button("Frame Selection")) ImGui::SetTooltip("Viewport focus is available from the active editor tool");
        ImGui::SameLine();
        if (ImGui::Button("Reset Layout")) ImGui::SetTooltip("Layout reset is handled by the editor workspace");
        ImGui::SameLine();
        ImGui::TextDisabled("Editor: %s", GetEditorType());
    }
};

} // namespace fbzz::editor
