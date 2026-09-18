/// @file    MapEditorPanel.hpp
/// @brief   Viewport 中心の Map Editing Mode で地形・水・植生ツールを集約するパネル。
/// @author  Hasegawa Jin
/// @date    2026-06-14
#pragma once

#include <Editor/Panels/EditorToolPanel.hpp>

namespace fbzz::editor {

/// MapEditorPanel — Map Tool の設定 UI を1ウィンドウへ集約する。ツールごとの独立ウィンドウを
/// 往復せず、Viewport と隣接した固定領域で編集する。
/// @note ツールの「選択」状態はビューポートのオーバーレイツールバー / 数字キーと共有する
///       (EditorContext::mapActiveTool)。このパネルは選択中ツールの詳細設定に集中する。
class MapEditorPanel final : public EditorToolPanel {
public:
    const char* GetWindowName()        const override { return "Map Tools"; }
    const char* GetEditorType()        const override { return "Map Editor"; }
    const char* GetViewMenuName()      const override { return "Map Tools"; }
    bool        ShowInViewMenu()       const override { return false; }
    bool        GetDefaultVisibility() const override { return false; }

protected:
    void OnRenderContent(EditorContext& ctx) override;
    bool CanClose() const override { return false; }

private:
    void DrawGridContent(EditorContext& ctx);

    int  m_gridSelectedX = -1;
    int  m_gridSelectedZ = -1;
};

} // namespace fbzz::editor
