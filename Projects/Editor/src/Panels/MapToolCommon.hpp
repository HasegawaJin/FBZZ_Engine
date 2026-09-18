/// @file    MapToolCommon.hpp
/// @brief   Map Editing Mode のツール切替を MapEditorPanel と ViewportPanel (オーバーレイ / 数字キー) で共有する。
/// @author  Hasegawa Jin
/// @date    2026-07-15
///
/// @note SetActive の切替がパネルごとに分散すると、片方から切り替えたときにもう片方の表示と食い違う。唯一の切替関数をここに置く。
#pragma once

#include <Editor/EditorContext.hpp>
#include "../Tools/TerrainTool.hpp"

namespace fbzz::editor {

/// @brief Map ツール定義 (ラベル・ツールチップ・ホットキー表示)。オーバーレイとパネルの両方が使う。
/// @note 数字キーは表の並び順から振る。既存のキー割り当てを変えないため追加は末尾へ置く。
struct MapToolDef {
    EditorContext::MapTool tool;
    const char*            label;
    const char*            shortcut; ///< 数字キー表示 ("1" 等)
    const char*            tooltip;
};

inline constexpr MapToolDef kMapToolDefs[] = {
    { EditorContext::MapTool::TerrainSculpt, "Sculpt",  "1", "Raise / lower / smooth / erode terrain height, or build a ramp\nShift+drag: Smooth, Ctrl+drag: Lower, [ ]: brush size" },
    { EditorContext::MapTool::TerrainPaint,  "Paint",   "2", "Paint splatmap texture layers onto the terrain" },
    { EditorContext::MapTool::Grid,          "Grid",    "3", "Manage terrain grid layout and cell assignment" },
    { EditorContext::MapTool::TerrainHole,   "Holes",   "4", "Cut or fill holes in the terrain surface\nCtrl+drag: invert" },
};

/// @return ツールが TerrainTool の入力を使うか。
inline bool IsTerrainMapTool(EditorContext::MapTool tool)
{
    return tool == EditorContext::MapTool::TerrainSculpt
        || tool == EditorContext::MapTool::TerrainPaint
        || tool == EditorContext::MapTool::TerrainHole;
}

/// @brief ツールを切り替え、各ツールのアクティブ状態を同期する。
inline void ActivateMapTool(EditorContext& ctx, EditorContext::MapTool tool)
{
    ctx.mapActiveTool = tool;
    if (ctx.terrainTool) {
        ctx.terrainTool->SetActive(IsTerrainMapTool(tool));
        TerrainTool::Mode mode = TerrainTool::Mode::Sculpt;
        if (tool == EditorContext::MapTool::TerrainPaint)     mode = TerrainTool::Mode::Paint;
        else if (tool == EditorContext::MapTool::TerrainHole) mode = TerrainTool::Mode::Hole;
        ctx.terrainTool->SetMode(mode);
    }
}

} // namespace fbzz::editor
