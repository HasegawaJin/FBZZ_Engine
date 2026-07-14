// FBZZ Engine
// EditorToolPanel.hpp | fbzz::editor
// Effect/Map等の制作ツールが共通利用するEditorパネル基底クラス
#pragma once

#include <Editor/Panels/IPanel.hpp>

namespace fbzz::editor {

// WHY: 個別ツールがWindow登録・共通ヘッダー・カテゴリ分けを重複実装すると、
//      新しいEditorを追加するたびにUI規約が分岐するため、制作ツール用の薄い共通層を設ける。
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
