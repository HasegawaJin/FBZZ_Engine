// FBZZ Engine
// MapEditorPanel.hpp | fbzz::editor
// Viewport 中心の Map Editing Mode で地形・水・植生ツールを集約するパネル
#pragma once

#include <Editor/Panels/IPanel.hpp>

namespace fbzz::editor {

// MapEditorPanel — Map Tool の選択と設定 UI を1ウィンドウへ集約する。
// WHY: ツールごとの独立ウィンドウを往復せず、Viewport と隣接した固定領域で編集するため。
class MapEditorPanel final : public IPanel {
public:
    const char* GetWindowName() const override { return "Map Tools"; }
    const char* GetViewMenuName() const override { return "Map Tools"; }
    bool ShowInViewMenu() const override { return false; }

protected:
    void OnRenderContent(EditorContext& ctx) override;
    bool CanClose() const override { return false; }

private:
    enum class Tool {
        TerrainSculpt,
        TerrainPaint,
        Water,
        Detail,
        Foliage
    };

    void ActivateTool(EditorContext& ctx, Tool tool);

    Tool m_activeTool = Tool::TerrainSculpt;
};

} // namespace fbzz::editor
