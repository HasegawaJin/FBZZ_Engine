/// @file    MapToolCommon.hpp
/// @brief   Map Editing Mode のツール切替を MapEditorPanel と ViewportPanel (オーバーレイ / 数字キー) で共有する。
/// @author  Hasegawa Jin
/// @date    2026-07-15
///
/// ツールの SetActive 切替がパネルごとに分散すると片方から切り替えたときにもう片方の表示状態と
/// 食い違うため、唯一の切替関数をここに置く。
#pragma once

#include <Editor/EditorContext.hpp>
#include "../Tools/TerrainTool.hpp"

namespace fbzz::editor {

/// Map ツール定義 (ラベル・ツールチップ・ホットキー表示)。オーバーレイとパネルの両方が使う。
struct MapToolDef {
    EditorContext::MapTool tool;
    const char*            label;
    const char*            shortcut; ///< 数字キー表示 ("1" 等)
    const char*            tooltip;
};

inline constexpr MapToolDef kMapToolDefs[] = {
    { EditorContext::MapTool::TerrainSculpt, "Sculpt",  "1", "Raise / lower / smooth terrain height with a brush\nShift+drag: Smooth, Ctrl+drag: Lower, [ ]: brush size" },
    { EditorContext::MapTool::TerrainPaint,  "Paint",   "2", "Paint splatmap texture layers onto the terrain" },
    { EditorContext::MapTool::Grid,          "Grid",    "3", "Manage terrain grid layout and cell assignment" },
};

/// ツールを切り替え、各ツールのアクティブ状態を同期する。
inline void ActivateMapTool(EditorContext& ctx, EditorContext::MapTool tool)
{
    ctx.mapActiveTool = tool;
    if (ctx.terrainTool) {
        const bool terrainActive =
            tool == EditorContext::MapTool::TerrainSculpt
            || tool == EditorContext::MapTool::TerrainPaint;
        ctx.terrainTool->SetActive(terrainActive);
        ctx.terrainTool->SetMode(
            tool == EditorContext::MapTool::TerrainPaint
                ? TerrainTool::Mode::Paint
                : TerrainTool::Mode::Sculpt);
    }
}

} // namespace fbzz::editor
