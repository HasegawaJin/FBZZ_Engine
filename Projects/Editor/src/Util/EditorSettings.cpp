// FBZZ Engine
// EditorSettings.cpp | fbzz::editor
// エディター設定の TOML 永続化実装
#include <Editor/Util/EditorSettings.hpp>
#include <toml++/toml.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Core/Logger.hpp>
#include <filesystem>
#include <sstream>

namespace fbzz::editor {

bool EditorSettings::Load(const std::string& path, const std::string& projectRoot)
{
    std::string text;
    if (!util::FileSystem::ReadText(path, text)) return false;

    // TOML_EXCEPTIONS=0 なので parse_result で受ける
    auto result = toml::parse(text);
    if (!result) {
        FBZZ_LOG_WARN("EditorSettings: parse failed: %s", path.c_str());
        return false;
    }
    auto& tbl = result.table();

    // カメラ
    if (auto v = tbl["camera"]["speed"].value<float>())       cameraSpeed       = *v;
    if (auto v = tbl["camera"]["sensitivity"].value<float>()) cameraSensitivity = *v;
    if (auto v = tbl["camera"]["last_pos_x"].value<float>())  cameraLastPx      = *v;
    if (auto v = tbl["camera"]["last_pos_y"].value<float>())  cameraLastPy      = *v;
    if (auto v = tbl["camera"]["last_pos_z"].value<float>())  cameraLastPz      = *v;
    if (auto v = tbl["camera"]["last_rot_x"].value<float>())  cameraLastRx      = *v;
    if (auto v = tbl["camera"]["last_rot_y"].value<float>())  cameraLastRy      = *v;
    if (auto v = tbl["camera"]["last_rot_z"].value<float>())  cameraLastRz      = *v;
    if (auto v = tbl["camera"]["last_rot_w"].value<float>())  cameraLastRw      = *v;

    // ビュー
    if (auto v = tbl["view"]["show_grid"].value<bool>())      showGrid      = *v;
    if (auto v = tbl["view"]["grid_size"].value<float>())     gridSize      = *v;
    if (auto v = tbl["view"]["show_light_range"].value<bool>()) showLightRange = *v;
    if (auto v = tbl["view"]["show_vfx_gizmos"].value<bool>()) showVFXGizmos = *v;
    if (auto v = tbl["view"]["show_skeleton"].value<bool>())  showSkeleton  = *v;
    if (auto v = tbl["view"]["show_stats"].value<bool>())     showStats = *v;
    if (auto v = tbl["view"]["scene_view_occlusion_culling"].value<bool>())
        sceneViewOcclusionCulling = *v;
    if (auto v = tbl["view"]["surface_snap_align_to_normal"].value<bool>())
        surfaceSnapAlignToNormal = *v;

    // スナップ
    if (auto v = tbl["snap"]["enabled"].value<bool>())   snapEnabled = *v;
    if (auto v = tbl["snap"]["pos"].value<float>())      snapPos     = *v;
    if (auto v = tbl["snap"]["rot"].value<float>())      snapRot     = *v;
    if (auto v = tbl["snap"]["scale"].value<float>())    snapScale   = *v;

    // ギズモ
    if (auto v = tbl["gizmo"]["mode"].value<int64_t>())       gizmoMode     = static_cast<int>(*v);
    if (auto v = tbl["gizmo"]["space"].value<int64_t>())      gizmoSpace    = static_cast<int>(*v);
    if (auto v = tbl["gizmo"]["pivot"].value<int64_t>())      gizmoPivot    = static_cast<int>(*v);

    // ゲームビュー
    if (auto v = tbl["game_view"]["aspect"].value<int64_t>()) gameViewportAspect = static_cast<int>(*v);
    if (auto v = tbl["game_view"]["play_focus_mode"].value<int64_t>()) playFocusMode = static_cast<int>(*v);

    // その他
    if (auto v = tbl["misc"]["hot_reload"].value<bool>()) hotReloadEnabled = *v;
    if (auto v = tbl["misc"]["ai_command_bus"].value<bool>()) aiCommandBusEnabled = *v;

    // ツールウィンドウ
    if (auto v = tbl["tools"]["show_terrain"].value<bool>()) showTerrainTool = *v;
    if (auto v = tbl["tools"]["show_water"].value<bool>())   showWaterTool   = *v;
    if (auto v = tbl["tools"]["show_detail"].value<bool>())  showDetailTool  = *v;
    if (auto v = tbl["tools"]["show_foliage"].value<bool>()) showFoliageTool = *v;

    // Map Mode フィルター
    if (auto v = tbl["map_mode"]["hierarchy_filter"].value<bool>()) mapHierarchyFilter = *v;
    if (auto v = tbl["map_mode"]["inspector_filter"].value<bool>()) mapInspectorFilter = *v;
    if (auto v = tbl["map_mode"]["active_tool"].value<int64_t>())   mapActiveTool = static_cast<int>(*v);

    // Hierarchy
    if (auto v = tbl["hierarchy"]["show_generated_objects"].value<bool>()) showGeneratedObjects = *v;

    // TerrainTool ブラシ設定
    if (auto v = tbl["terrain_tool"]["brush_radius"].value<float>())    terrainBrushRadius   = *v;
    if (auto v = tbl["terrain_tool"]["brush_strength"].value<float>())  terrainBrushStrength = *v;
    if (auto v = tbl["terrain_tool"]["brush_falloff"].value<int64_t>()) terrainBrushFalloff  = static_cast<int>(*v);
    if (auto v = tbl["terrain_tool"]["sculpt_mode"].value<int64_t>())   terrainSculptMode    = static_cast<int>(*v);
    if (auto v = tbl["terrain_tool"]["paint_layer"].value<int64_t>())   terrainPaintLayer    = static_cast<uint32_t>(*v);

    // DetailTool ブラシ設定
    if (auto v = tbl["detail_tool"]["brush_radius"].value<float>())      detailBrushRadius    = *v;
    if (auto v = tbl["detail_tool"]["brush_strength"].value<float>())    detailBrushStrength  = *v;
    if (auto v = tbl["detail_tool"]["mode"].value<int64_t>())            detailMode           = static_cast<int>(*v);
    if (auto v = tbl["detail_tool"]["layer_index"].value<int64_t>())     detailLayerIndex     = static_cast<int>(*v);
    if (auto v = tbl["detail_tool"]["show_chunk_bounds"].value<bool>())  detailShowChunkBounds = *v;
    if (auto v = tbl["detail_tool"]["show_counts"].value<bool>())        detailShowCounts      = *v;

    // FoliageTool ブラシ設定
    if (auto v = tbl["foliage_tool"]["erase_radius"].value<float>())     foliageEraseRadius    = *v;

    // Camera Bookmarks
    if (auto* arr = tbl["camera_bookmarks"].as_array()) {
        for (std::size_t i = 0; i < arr->size() && i < cameraBookmarks.size(); ++i) {
            auto* t = (*arr)[i].as_table();
            if (!t) continue;
            auto& bk = cameraBookmarks[i];
            if (auto v = (*t)["valid"].value<bool>())  bk.valid = *v;
            if (auto v = (*t)["px"].value<float>())    bk.px = *v;
            if (auto v = (*t)["py"].value<float>())    bk.py = *v;
            if (auto v = (*t)["pz"].value<float>())    bk.pz = *v;
            if (auto v = (*t)["rx"].value<float>())    bk.rx = *v;
            if (auto v = (*t)["ry"].value<float>())    bk.ry = *v;
            if (auto v = (*t)["rz"].value<float>())    bk.rz = *v;
            if (auto v = (*t)["rw"].value<float>())    bk.rw = *v;
        }
    }

    // デフォルトインポート設定
    if (auto v = tbl["import"]["source_dcc"].value<int64_t>())
        defaultImportOptions.sourceDcc = static_cast<fbzz::editor::FbxSourceDcc>(*v);
    if (auto v = tbl["import"]["up_axis"].value<int64_t>())
        defaultImportOptions.upAxis = static_cast<fbzz::editor::FbxUpAxis>(*v);
    if (auto v = tbl["import"]["normal_map_convention"].value<int64_t>())
        defaultImportOptions.normalMapConvention = static_cast<fbzz::editor::NormalMapConvention>(*v);
    if (auto v = tbl["import"]["unit_scale_multiplier"].value<float>())
        defaultImportOptions.unitScaleMultiplier = *v;
    if (auto v = tbl["import"]["generate_normals"].value<bool>())
        defaultImportOptions.generateNormals = *v;
    if (auto v = tbl["import"]["generate_tangents"].value<bool>())
        defaultImportOptions.generateTangents = *v;
    if (auto v = tbl["import"]["generate_tex_descriptors"].value<bool>())
        defaultImportOptions.generateTexDescriptors = *v;
    if (auto v = tbl["import"]["default_compression"].value<int64_t>())
        defaultImportOptions.defaultCompression = static_cast<fbzz::asset::TextureCompression>(*v);

    // ホットキーオーバーライド
    hotkeyOverrides.clear();
    if (auto* arr = tbl["hotkeys"]["overrides"].as_array()) {
        for (auto& elem : *arr) {
            if (auto* t = elem.as_table()) {
                HotkeyOverride ov;
                if (auto v = (*t)["name"].value<std::string>()) ov.name  = *v;
                if (auto v = (*t)["key"].value<int64_t>())       ov.key   = static_cast<int>(*v);
                if (auto v = (*t)["ctrl"].value<bool>())          ov.ctrl  = *v;
                if (auto v = (*t)["shift"].value<bool>())         ov.shift = *v;
                if (auto v = (*t)["alt"].value<bool>())           ov.alt   = *v;
                if (!ov.name.empty()) hotkeyOverrides.push_back(std::move(ov));
            }
        }
    }

    // UI
    if (auto v = tbl["ui"]["scale"].value<float>()) editorUiScale = *v;
    if (auto v = tbl["ui"]["multi_viewport"].value<bool>()) multiViewportEnabled = *v;

    // 相対パスで保存されたパスを projectRoot と組み合わせて絶対パスへ戻すヘルパー。
    const auto toAbsProjectPath = [&projectRoot](std::string p) -> std::string {
        if (!projectRoot.empty() && !p.empty()) {
            std::filesystem::path fp = util::FileSystem::PathFromUtf8(p);
            if (fp.is_relative()) {
                const auto abs = util::FileSystem::MakeAbsolute(
                    util::FileSystem::PathFromUtf8(projectRoot) / fp);
                return util::FileSystem::PathToUtf8(abs);
            }
        }
        return p;
    };

    // Asset Browser
    if (auto v = tbl["asset_browser"]["icon_size"].value<float>()) assetBrowserIconSize = *v;
    if (auto v = tbl["asset_browser"]["tree_width"].value<float>()) assetBrowserTreeWidth = *v;
    if (auto v = tbl["asset_browser"]["view_mode"].value<int64_t>())   assetBrowserViewMode   = static_cast<int>(*v);
    if (auto v = tbl["asset_browser"]["sort_mode"].value<int64_t>())   assetBrowserSortMode   = static_cast<int>(*v);
    if (auto v = tbl["asset_browser"]["type_filter"].value<int64_t>()) assetBrowserTypeFilter = static_cast<int>(*v);
    if (auto v = tbl["asset_browser"]["search_all_folders"].value<bool>()) assetBrowserSearchAllFolders = *v;
    if (auto v = tbl["asset_browser"]["current_folder"].value<std::string>())
        assetBrowserCurrentFolder = toAbsProjectPath(*v);
    assetBrowserBookmarks.clear();
    if (auto* arr = tbl["asset_browser"]["bookmarks"].as_array()) {
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>()) assetBrowserBookmarks.push_back(*v);
    }

    // Console
    if (auto v = tbl["console"]["show_debug"].value<bool>())    consoleShowDebug   = *v;
    if (auto v = tbl["console"]["show_info"].value<bool>())     consoleShowInfo    = *v;
    if (auto v = tbl["console"]["show_warn"].value<bool>())     consoleShowWarn    = *v;
    if (auto v = tbl["console"]["show_error"].value<bool>())    consoleShowError   = *v;
    if (auto v = tbl["console"]["auto_scroll"].value<bool>())   consoleAutoScroll  = *v;
    if (auto v = tbl["console"]["collapse"].value<bool>())      consoleCollapse    = *v;
    if (auto v = tbl["console"]["clear_on_play"].value<bool>()) consoleClearOnPlay = *v;
    if (auto v = tbl["console"]["show_detail"].value<bool>())   consoleShowDetail  = *v;

    // パネル表示状態
    panelVisibility.clear();
    if (auto* arr = tbl["panels"]["visible"].as_array()) {
        for (auto& elem : *arr) {
            if (auto* t = elem.as_table()) {
                auto name = (*t)["name"].value<std::string>();
                auto open = (*t)["open"].value<bool>();
                if (name && !name->empty() && open)
                    panelVisibility.emplace_back(*name, *open);
            }
        }
    }

    // Debug メニュー - レンダリングオーバーレイ
    if (auto v = tbl["debug"]["show_colliders"].value<bool>())         showColliders        = *v;
    if (auto v = tbl["debug"]["show_ui_rects"].value<bool>())          showUIRects          = *v;
    if (auto v = tbl["debug"]["show_terrain_collision"].value<bool>()) showTerrainCollision = *v;
    if (auto v = tbl["debug"]["show_decal_bounds"].value<bool>())      showDecalBounds      = *v;
    if (auto v = tbl["debug"]["show_navmesh"].value<bool>())           showNavMesh          = *v;
    if (auto v = tbl["debug"]["show_nav_sensors"].value<bool>())       showNavSensors       = *v;
    if (auto v = tbl["debug"]["navmesh_draw_mode"].value<int64_t>())   navMeshDrawMode      = static_cast<int>(*v);
    if (auto v = tbl["debug"]["navmesh_draw_distance"].value<double>()) navMeshDrawDistance = static_cast<float>(*v);
    if (auto v = tbl["debug"]["view_mode"].value<int64_t>())      viewMode = static_cast<int>(*v);

    // Inspector セクション折り畳み状態
    inspectorSectionState.clear();
    if (auto* arr = tbl["inspector_sections"]["states"].as_array()) {
        for (auto& elem : *arr) {
            if (auto* t = elem.as_table()) {
                auto k = (*t)["k"].value<int64_t>();
                auto v = (*t)["v"].value<bool>();
                if (k && v)
                    inspectorSectionState.emplace_back(
                        static_cast<uint32_t>(*k), *v);
            }
        }
    }

    // シーン
    if (auto v = tbl["scene"]["last_path"].value<std::string>())
        lastScenePath = toAbsProjectPath(*v);

    // 最近開いたシーン
    recentScenes.clear();
    if (auto* arr = tbl["scene"]["recent"].as_array()) {
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>())
                recentScenes.push_back(toAbsProjectPath(*v));
    }

    // オートセーブ
    if (auto v = tbl["autosave"]["enabled"].value<bool>())       autoSaveEnabled     = *v;
    if (auto v = tbl["autosave"]["interval_sec"].value<int64_t>()) autoSaveIntervalSec = static_cast<int>(*v);

    return true;
}

bool EditorSettings::Save(const std::string& path, const std::string& projectRoot) const
{
    // カメラ
    toml::table camTbl;
    camTbl.insert("speed",       cameraSpeed);
    camTbl.insert("sensitivity", cameraSensitivity);
    camTbl.insert("last_pos_x",  cameraLastPx);
    camTbl.insert("last_pos_y",  cameraLastPy);
    camTbl.insert("last_pos_z",  cameraLastPz);
    camTbl.insert("last_rot_x",  cameraLastRx);
    camTbl.insert("last_rot_y",  cameraLastRy);
    camTbl.insert("last_rot_z",  cameraLastRz);
    camTbl.insert("last_rot_w",  cameraLastRw);

    // ビュー
    toml::table viewTbl;
    viewTbl.insert("show_grid",        showGrid);
    viewTbl.insert("grid_size",        gridSize);
    viewTbl.insert("show_light_range", showLightRange);
    viewTbl.insert("show_vfx_gizmos", showVFXGizmos);
    viewTbl.insert("show_skeleton",    showSkeleton);
    viewTbl.insert("show_stats",       showStats);
    viewTbl.insert("scene_view_occlusion_culling", sceneViewOcclusionCulling);
    viewTbl.insert("surface_snap_align_to_normal", surfaceSnapAlignToNormal);

    // スナップ
    toml::table snapTbl;
    snapTbl.insert("enabled", snapEnabled);
    snapTbl.insert("pos",     snapPos);
    snapTbl.insert("rot",     snapRot);
    snapTbl.insert("scale",   snapScale);

    // ギズモ
    toml::table gizmoTbl;
    gizmoTbl.insert("mode",  static_cast<int64_t>(gizmoMode));
    gizmoTbl.insert("space", static_cast<int64_t>(gizmoSpace));
    gizmoTbl.insert("pivot", static_cast<int64_t>(gizmoPivot));

    // ゲームビュー
    toml::table gameViewTbl;
    gameViewTbl.insert("aspect", static_cast<int64_t>(gameViewportAspect));
    gameViewTbl.insert("play_focus_mode", static_cast<int64_t>(playFocusMode));

    // その他
    toml::table miscTbl;
    miscTbl.insert("hot_reload", hotReloadEnabled);
    miscTbl.insert("ai_command_bus", aiCommandBusEnabled);

    // ツールウィンドウ
    toml::table toolsTbl;
    toolsTbl.insert("show_terrain", showTerrainTool);
    toolsTbl.insert("show_water",   showWaterTool);
    toolsTbl.insert("show_detail",  showDetailTool);
    toolsTbl.insert("show_foliage", showFoliageTool);

    // Map Mode フィルター
    toml::table mapModeTbl;
    mapModeTbl.insert("hierarchy_filter", mapHierarchyFilter);
    mapModeTbl.insert("inspector_filter", mapInspectorFilter);
    mapModeTbl.insert("active_tool",      static_cast<int64_t>(mapActiveTool));

    // Hierarchy
    toml::table hierarchyTbl;
    hierarchyTbl.insert("show_generated_objects", showGeneratedObjects);

    // TerrainTool ブラシ設定
    toml::table terrainToolTbl;
    terrainToolTbl.insert("brush_radius",   terrainBrushRadius);
    terrainToolTbl.insert("brush_strength", terrainBrushStrength);
    terrainToolTbl.insert("brush_falloff",  static_cast<int64_t>(terrainBrushFalloff));
    terrainToolTbl.insert("sculpt_mode",    static_cast<int64_t>(terrainSculptMode));
    terrainToolTbl.insert("paint_layer",    static_cast<int64_t>(terrainPaintLayer));

    // DetailTool ブラシ設定
    toml::table detailToolTbl;
    detailToolTbl.insert("brush_radius",      detailBrushRadius);
    detailToolTbl.insert("brush_strength",    detailBrushStrength);
    detailToolTbl.insert("mode",              static_cast<int64_t>(detailMode));
    detailToolTbl.insert("layer_index",       static_cast<int64_t>(detailLayerIndex));
    detailToolTbl.insert("show_chunk_bounds", detailShowChunkBounds);
    detailToolTbl.insert("show_counts",       detailShowCounts);

    // FoliageTool ブラシ設定
    toml::table foliageToolTbl;
    foliageToolTbl.insert("erase_radius", foliageEraseRadius);

    // Camera Bookmarks
    toml::array camBkArr;
    for (const auto& bk : cameraBookmarks) {
        toml::table t;
        t.insert("valid", bk.valid);
        t.insert("px", bk.px); t.insert("py", bk.py); t.insert("pz", bk.pz);
        t.insert("rx", bk.rx); t.insert("ry", bk.ry);
        t.insert("rz", bk.rz); t.insert("rw", bk.rw);
        camBkArr.push_back(std::move(t));
    }

    // 絶対パスを projectRoot 相対へ変換して保存する (プロジェクトを移動しても壊れない)。
    const auto toRelProjectPath = [&projectRoot](const std::string& p) -> std::string {
        if (!projectRoot.empty() && !p.empty()) {
            const std::filesystem::path rel = util::FileSystem::RelativePath(
                util::FileSystem::PathFromUtf8(p),
                util::FileSystem::PathFromUtf8(projectRoot));
            if (!rel.empty())
                return rel.generic_string();
        }
        return p;
    };

    // Asset Browser
    toml::table assetBrowserTbl;
    assetBrowserTbl.insert("icon_size", assetBrowserIconSize);
    assetBrowserTbl.insert("tree_width", assetBrowserTreeWidth);
    assetBrowserTbl.insert("view_mode",   static_cast<int64_t>(assetBrowserViewMode));
    assetBrowserTbl.insert("sort_mode",   static_cast<int64_t>(assetBrowserSortMode));
    assetBrowserTbl.insert("type_filter", static_cast<int64_t>(assetBrowserTypeFilter));
    assetBrowserTbl.insert("search_all_folders", assetBrowserSearchAllFolders);
    assetBrowserTbl.insert("current_folder", toRelProjectPath(assetBrowserCurrentFolder));
    {
        toml::array bkArr;
        for (const auto& bk : assetBrowserBookmarks) bkArr.push_back(bk);
        assetBrowserTbl.insert("bookmarks", std::move(bkArr));
    }

    // Console
    toml::table consoleTbl;
    consoleTbl.insert("show_debug",    consoleShowDebug);
    consoleTbl.insert("show_info",     consoleShowInfo);
    consoleTbl.insert("show_warn",     consoleShowWarn);
    consoleTbl.insert("show_error",    consoleShowError);
    consoleTbl.insert("auto_scroll",   consoleAutoScroll);
    consoleTbl.insert("collapse",      consoleCollapse);
    consoleTbl.insert("clear_on_play", consoleClearOnPlay);
    consoleTbl.insert("show_detail",   consoleShowDetail);

    // パネル表示状態
    toml::array panelArr;
    for (const auto& [name, open] : panelVisibility) {
        toml::table entry;
        entry.insert("name", name);
        entry.insert("open", open);
        panelArr.push_back(std::move(entry));
    }
    toml::table panelsTbl;
    panelsTbl.insert("visible", std::move(panelArr));

    // Inspector セクション折り畳み状態
    toml::array statesArr;
    for (auto& [key, open] : inspectorSectionState) {
        toml::table entry;
        entry.insert("k", static_cast<int64_t>(key));
        entry.insert("v", open);
        statesArr.push_back(std::move(entry));
    }
    toml::table inspectorTbl;
    inspectorTbl.insert("states", std::move(statesArr));

    // シーン
    toml::table sceneTbl;
    sceneTbl.insert("last_path", toRelProjectPath(lastScenePath));
    {
        toml::array recentArr;
        for (const auto& s : recentScenes) recentArr.push_back(toRelProjectPath(s));
        sceneTbl.insert("recent", std::move(recentArr));
    }

    // オートセーブ
    toml::table autosaveTbl;
    autosaveTbl.insert("enabled",      autoSaveEnabled);
    autosaveTbl.insert("interval_sec", static_cast<int64_t>(autoSaveIntervalSec));

    // Debug メニュー - レンダリングオーバーレイ
    toml::table debugTbl;
    debugTbl.insert("show_colliders",         showColliders);
    debugTbl.insert("show_ui_rects",          showUIRects);
    debugTbl.insert("show_terrain_collision", showTerrainCollision);
    debugTbl.insert("show_decal_bounds",      showDecalBounds);
    debugTbl.insert("show_navmesh",           showNavMesh);
    debugTbl.insert("show_nav_sensors",       showNavSensors);
    debugTbl.insert("navmesh_draw_mode",      static_cast<int64_t>(navMeshDrawMode));
    debugTbl.insert("navmesh_draw_distance",  static_cast<double>(navMeshDrawDistance));
    debugTbl.insert("view_mode", static_cast<int64_t>(viewMode));

    // ホットキーオーバーライド
    toml::array ovArr;
    for (const auto& ov : hotkeyOverrides) {
        toml::table t;
        t.insert("name",  ov.name);
        t.insert("key",   static_cast<int64_t>(ov.key));
        t.insert("ctrl",  ov.ctrl);
        t.insert("shift", ov.shift);
        t.insert("alt",   ov.alt);
        ovArr.push_back(std::move(t));
    }
    toml::table hkTbl;
    hkTbl.insert("overrides", std::move(ovArr));

    // デフォルトインポート設定
    toml::table importTbl;
    importTbl.insert("source_dcc",                static_cast<int64_t>(defaultImportOptions.sourceDcc));
    importTbl.insert("up_axis",                   static_cast<int64_t>(defaultImportOptions.upAxis));
    importTbl.insert("normal_map_convention",    static_cast<int64_t>(defaultImportOptions.normalMapConvention));
    importTbl.insert("unit_scale_multiplier",    defaultImportOptions.unitScaleMultiplier);
    importTbl.insert("generate_normals",         defaultImportOptions.generateNormals);
    importTbl.insert("generate_tangents",        defaultImportOptions.generateTangents);
    importTbl.insert("generate_tex_descriptors", defaultImportOptions.generateTexDescriptors);
    importTbl.insert("default_compression",      static_cast<int64_t>(defaultImportOptions.defaultCompression));

    // UI スケール
    toml::table uiTbl;
    uiTbl.insert("scale", editorUiScale);
    uiTbl.insert("multi_viewport", multiViewportEnabled);

    toml::table root;
    root.insert("ui",                 std::move(uiTbl));
    root.insert("camera",             std::move(camTbl));
    root.insert("view",               std::move(viewTbl));
    root.insert("snap",               std::move(snapTbl));
    root.insert("gizmo",              std::move(gizmoTbl));
    root.insert("game_view",          std::move(gameViewTbl));
    root.insert("misc",               std::move(miscTbl));
    root.insert("tools",              std::move(toolsTbl));
    root.insert("map_mode",           std::move(mapModeTbl));
    root.insert("hierarchy",          std::move(hierarchyTbl));
    root.insert("terrain_tool",       std::move(terrainToolTbl));
    root.insert("detail_tool",        std::move(detailToolTbl));
    root.insert("foliage_tool",       std::move(foliageToolTbl));
    root.insert("camera_bookmarks",   std::move(camBkArr));
    root.insert("hotkeys",            std::move(hkTbl));
    root.insert("import",             std::move(importTbl));
    root.insert("asset_browser",      std::move(assetBrowserTbl));
    root.insert("console",            std::move(consoleTbl));
    root.insert("panels",             std::move(panelsTbl));
    root.insert("inspector_sections", std::move(inspectorTbl));
    root.insert("scene",              std::move(sceneTbl));
    root.insert("autosave",           std::move(autosaveTbl));
    root.insert("debug",              std::move(debugTbl));

    std::ostringstream ss;
    ss << root;
    return util::FileSystem::WriteText(path, ss.str());
}

} // namespace fbzz::editor
