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

    // ビュー
    if (auto v = tbl["view"]["show_grid"].value<bool>())      showGrid      = *v;
    if (auto v = tbl["view"]["grid_size"].value<float>())     gridSize      = *v;
    if (auto v = tbl["view"]["show_light_range"].value<bool>()) showLightRange = *v;
    if (auto v = tbl["view"]["show_skeleton"].value<bool>())  showSkeleton  = *v;
    if (auto v = tbl["view"]["show_stats"].value<bool>())     showStats = *v;

    // スナップ
    if (auto v = tbl["snap"]["enabled"].value<bool>())        snapEnabled   = *v;
    if (auto v = tbl["snap"]["distance"].value<float>())      snapDistance  = *v;

    // ギズモ
    if (auto v = tbl["gizmo"]["mode"].value<int64_t>())       gizmoMode     = static_cast<int>(*v);
    if (auto v = tbl["gizmo"]["space"].value<int64_t>())      gizmoSpace    = static_cast<int>(*v);

    // ゲームビュー
    if (auto v = tbl["game_view"]["aspect"].value<int64_t>()) gameViewportAspect = static_cast<int>(*v);

    // その他
    if (auto v = tbl["misc"]["hot_reload"].value<bool>()) hotReloadEnabled = *v;

    // ツールウィンドウ
    if (auto v = tbl["tools"]["show_terrain"].value<bool>()) showTerrainTool = *v;
    if (auto v = tbl["tools"]["show_water"].value<bool>())   showWaterTool   = *v;
    if (auto v = tbl["tools"]["show_detail"].value<bool>())  showDetailTool  = *v;
    if (auto v = tbl["tools"]["show_foliage"].value<bool>()) showFoliageTool = *v;

    // Map Mode フィルター
    if (auto v = tbl["map_mode"]["hierarchy_filter"].value<bool>()) mapHierarchyFilter = *v;
    if (auto v = tbl["map_mode"]["inspector_filter"].value<bool>()) mapInspectorFilter = *v;

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

    // Asset Browser
    if (auto v = tbl["asset_browser"]["icon_size"].value<float>()) assetBrowserIconSize = *v;

    // Debug メニュー - レンダリングオーバーレイ
    if (auto v = tbl["debug"]["show_colliders"].value<bool>())    showColliders   = *v;
    if (auto v = tbl["debug"]["show_decal_bounds"].value<bool>()) showDecalBounds = *v;
    if (auto v = tbl["debug"]["view_mode"].value<int64_t>())      viewMode        = static_cast<int>(*v);
    if (auto v = tbl["debug"]["shadow"].value<bool>())            shadowEnabled   = *v;

    // Debug メニュー - Post Process
    if (auto v = tbl["post_process"]["fxaa"].value<bool>())              ppFxaaEnabled              = *v;
    if (auto v = tbl["post_process"]["exposure"].value<float>())          ppExposure                 = *v;
    if (auto v = tbl["post_process"]["bloom"].value<bool>())              ppBloomEnabled             = *v;
    if (auto v = tbl["post_process"]["bloom_intensity"].value<float>())   ppBloomIntensity           = *v;
    if (auto v = tbl["post_process"]["ao"].value<bool>())                 ppAoEnabled                = *v;
    if (auto v = tbl["post_process"]["fog"].value<bool>())                ppFogEnabled               = *v;
    if (auto v = tbl["post_process"]["fog_density"].value<float>())       ppFogDensity               = *v;
    if (auto v = tbl["post_process"]["fog_far"].value<float>())           ppFogFar                   = *v;
    if (auto v = tbl["post_process"]["color_grading"].value<bool>())      ppColorGradingEnabled      = *v;
    if (auto v = tbl["post_process"]["contrast"].value<float>())          ppContrast                 = *v;
    if (auto v = tbl["post_process"]["saturation"].value<float>())        ppSaturation               = *v;
    if (auto v = tbl["post_process"]["hue_shift"].value<float>())         ppHueShift                 = *v;
    if (auto v = tbl["post_process"]["vignette"].value<bool>())           ppVignetteEnabled          = *v;
    if (auto v = tbl["post_process"]["film_grain"].value<bool>())         ppFilmGrainEnabled         = *v;
    if (auto v = tbl["post_process"]["sharpen"].value<bool>())            ppSharpenEnabled           = *v;
    if (auto v = tbl["post_process"]["sharpen_strength"].value<float>())  ppSharpenStrength          = *v;
    if (auto v = tbl["post_process"]["dof"].value<bool>())                ppDofEnabled               = *v;
    if (auto v = tbl["post_process"]["dof_focus"].value<float>())         ppDofFocus                 = *v;
    if (auto v = tbl["post_process"]["dof_blur"].value<float>())          ppDofBlur                  = *v;
    if (auto v = tbl["post_process"]["chromatic_aberration"].value<bool>()) ppChromaticAberrationEnabled = *v;
    if (auto v = tbl["post_process"]["lens_distortion"].value<bool>())    ppLensDistortionEnabled    = *v;
    if (auto v = tbl["post_process"]["sepia"].value<bool>())              ppSepiaEnabled             = *v;
    if (auto v = tbl["post_process"]["invert"].value<bool>())             ppInvertEnabled            = *v;
    if (auto v = tbl["post_process"]["posterize"].value<bool>())          ppPosterizeEnabled         = *v;
    if (auto v = tbl["post_process"]["pixelate"].value<bool>())           ppPixelateEnabled          = *v;
    if (auto v = tbl["post_process"]["posterize_levels"].value<float>())  ppPosterizeLevels          = *v;
    if (auto v = tbl["post_process"]["pixel_size"].value<float>())        ppPixelSize                = *v;

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
    if (auto v = tbl["scene"]["last_path"].value<std::string>()) {
        lastScenePath = *v;
        // 相対パスで保存されていた場合は projectRoot と組み合わせて絶対パスに戻す
        if (!projectRoot.empty() && !lastScenePath.empty()) {
            std::filesystem::path p = util::FileSystem::PathFromUtf8(lastScenePath);
            if (p.is_relative()) {
                const auto abs = util::FileSystem::MakeAbsolute(
                    util::FileSystem::PathFromUtf8(projectRoot) / p);
                lastScenePath = util::FileSystem::PathToUtf8(abs);
            }
        }
    }

    return true;
}

bool EditorSettings::Save(const std::string& path, const std::string& projectRoot) const
{
    // カメラ
    toml::table camTbl;
    camTbl.insert("speed",       cameraSpeed);
    camTbl.insert("sensitivity", cameraSensitivity);

    // ビュー
    toml::table viewTbl;
    viewTbl.insert("show_grid",        showGrid);
    viewTbl.insert("grid_size",        gridSize);
    viewTbl.insert("show_light_range", showLightRange);
    viewTbl.insert("show_skeleton",    showSkeleton);
    viewTbl.insert("show_stats",       showStats);

    // スナップ
    toml::table snapTbl;
    snapTbl.insert("enabled",  snapEnabled);
    snapTbl.insert("distance", snapDistance);

    // ギズモ
    toml::table gizmoTbl;
    gizmoTbl.insert("mode",  static_cast<int64_t>(gizmoMode));
    gizmoTbl.insert("space", static_cast<int64_t>(gizmoSpace));

    // ゲームビュー
    toml::table gameViewTbl;
    gameViewTbl.insert("aspect", static_cast<int64_t>(gameViewportAspect));

    // その他
    toml::table miscTbl;
    miscTbl.insert("hot_reload", hotReloadEnabled);

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

    // Asset Browser
    toml::table assetBrowserTbl;
    assetBrowserTbl.insert("icon_size", assetBrowserIconSize);

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
    std::string scenePathToSave = lastScenePath;
    if (!projectRoot.empty() && !lastScenePath.empty()) {
        const std::filesystem::path rel = util::FileSystem::RelativePath(
            util::FileSystem::PathFromUtf8(lastScenePath),
            util::FileSystem::PathFromUtf8(projectRoot));
        if (!rel.empty())
            scenePathToSave = rel.generic_string();
    }

    toml::table sceneTbl;
    sceneTbl.insert("last_path", scenePathToSave);

    // Debug メニュー - レンダリングオーバーレイ
    toml::table debugTbl;
    debugTbl.insert("show_colliders",    showColliders);
    debugTbl.insert("show_decal_bounds", showDecalBounds);
    debugTbl.insert("view_mode",         static_cast<int64_t>(viewMode));
    debugTbl.insert("shadow",            shadowEnabled);

    // Debug メニュー - Post Process
    toml::table ppTbl;
    ppTbl.insert("fxaa",                ppFxaaEnabled);
    ppTbl.insert("exposure",            ppExposure);
    ppTbl.insert("bloom",               ppBloomEnabled);
    ppTbl.insert("bloom_intensity",     ppBloomIntensity);
    ppTbl.insert("ao",                  ppAoEnabled);
    ppTbl.insert("fog",                 ppFogEnabled);
    ppTbl.insert("fog_density",         ppFogDensity);
    ppTbl.insert("fog_far",             ppFogFar);
    ppTbl.insert("color_grading",       ppColorGradingEnabled);
    ppTbl.insert("contrast",            ppContrast);
    ppTbl.insert("saturation",          ppSaturation);
    ppTbl.insert("hue_shift",           ppHueShift);
    ppTbl.insert("vignette",            ppVignetteEnabled);
    ppTbl.insert("film_grain",          ppFilmGrainEnabled);
    ppTbl.insert("sharpen",             ppSharpenEnabled);
    ppTbl.insert("sharpen_strength",    ppSharpenStrength);
    ppTbl.insert("dof",                 ppDofEnabled);
    ppTbl.insert("dof_focus",           ppDofFocus);
    ppTbl.insert("dof_blur",            ppDofBlur);
    ppTbl.insert("chromatic_aberration",ppChromaticAberrationEnabled);
    ppTbl.insert("lens_distortion",     ppLensDistortionEnabled);
    ppTbl.insert("sepia",               ppSepiaEnabled);
    ppTbl.insert("invert",              ppInvertEnabled);
    ppTbl.insert("posterize",           ppPosterizeEnabled);
    ppTbl.insert("pixelate",            ppPixelateEnabled);
    ppTbl.insert("posterize_levels",    ppPosterizeLevels);
    ppTbl.insert("pixel_size",          ppPixelSize);

    toml::table root;
    root.insert("camera",       std::move(camTbl));
    root.insert("view",         std::move(viewTbl));
    root.insert("snap",         std::move(snapTbl));
    root.insert("gizmo",        std::move(gizmoTbl));
    root.insert("game_view",    std::move(gameViewTbl));
    root.insert("misc",         std::move(miscTbl));
    root.insert("tools",        std::move(toolsTbl));
    root.insert("map_mode",     std::move(mapModeTbl));
    root.insert("terrain_tool", std::move(terrainToolTbl));
    root.insert("detail_tool",  std::move(detailToolTbl));
    root.insert("asset_browser",      std::move(assetBrowserTbl));
    root.insert("inspector_sections", std::move(inspectorTbl));
    root.insert("scene",              std::move(sceneTbl));
    root.insert("debug",              std::move(debugTbl));
    root.insert("post_process",       std::move(ppTbl));

    std::ostringstream ss;
    ss << root;
    return util::FileSystem::WriteText(path, ss.str());
}

} // namespace fbzz::editor
