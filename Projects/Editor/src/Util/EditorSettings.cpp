/// @file    EditorSettings.cpp
/// @brief   エディター設定の TOML 永続化実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include <Editor/Util/EditorSettings.hpp>
#include <toml++/toml.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Core/Logger.hpp>
#include <algorithm>
#include <filesystem>
#include <sstream>

namespace fbzz::editor {
namespace {

/// 個人の作業状態を置くファイル。Library は .gitignore 済みなので、clone しても
/// 他人の視点位置・開いていたシーン・パネル配置が付いてこない (Unity の UserSettings/ 相当)。
/// projectRoot が空 (プロジェクト未確定) なら保存先を決められないので空文字列を返す。
std::string EditorLocalStatePath(const std::string& projectRoot)
{
    if (projectRoot.empty()) return {};
    std::string root = projectRoot;
    if (root.back() != '/' && root.back() != '\\') root.push_back('/');
    return root + "Library/EditorLocalState.toml";
}

/// 共有設定を読んだテーブルの上へ、個人状態のセクションを被せる。
/// @note セクションごと差し替える: Load 本体は `tbl["scene"]` のような参照を 250 行にわたって持つため、
///       ファイル分割を読み出し側へ持ち込まず読む前に 1 つのテーブルへ合流させる。旧ファイルに同じセクションが残っていても local 側が勝つ。
void OverlayEditorLocalState(const std::string& projectRoot, toml::table& tbl)
{
    const std::string localPath = EditorLocalStatePath(projectRoot);
    if (localPath.empty()) return;

    std::string text;
    if (!util::FileSystem::ReadText(localPath, text)) return;

    auto result = toml::parse(text);
    if (!result) {
        /// @note 壊れていても復旧は要らない。次の保存で書き直され、失うのは作業状態だけ。
        FBZZ_LOG_WARN("EditorSettings: parse failed: %s", localPath.c_str());
        return;
    }

    for (const auto& [key, value] : result.table()) {
        if (const toml::table* section = value.as_table())
            tbl.insert_or_assign(key.str(), *section);
        else if (const toml::array* array = value.as_array())
            tbl.insert_or_assign(key.str(), *array);
    }
}

/// 個人状態のセクションを Library/EditorLocalState.toml へ書き出す。
bool SaveEditorLocalState(const std::string& projectRoot, toml::table&& localRoot)
{
    const std::string localPath = EditorLocalStatePath(projectRoot);
    if (localPath.empty()) return false;

    std::ostringstream ss;
    ss << "# 自動生成 — このマシンのエディター作業状態。git には載せない (Library は .gitignore 済み)。\n"
          "# 視点位置・開いていたシーン・パネル配置などは触るたびに変わり、人ごとに違う。\n"
          "# 共有ファイルへ置くと、同じプロジェクトを触る全員がこの行で衝突し続ける。\n\n";
    ss << localRoot;

    util::FileSystem::EnsureDirectory(util::FileSystem::GetDirectory(localPath));
    if (util::FileSystem::WriteText(localPath, ss.str())) return true;
    FBZZ_LOG_WARN("EditorSettings: local state write failed: %s", localPath.c_str());
    return false;
}

} // namespace

bool EditorSettings::Load(const std::string& path, const std::string& projectRoot)
{
    toml::table tbl;
    bool sharedLoaded = false;

    std::string text;
    if (util::FileSystem::ReadText(path, text)) {
        /// @note TOML_EXCEPTIONS=0 なので parse_result で受ける
        auto result = toml::parse(text);
        if (!result) {
            FBZZ_LOG_WARN("EditorSettings: parse failed: %s", path.c_str());
            return false;
        }
        tbl = std::move(result.table());
        sharedLoaded = true;
    }
    /// @note 共有設定がまだ無いプロジェクトでも読み出しは続ける。空テーブル相手なら
    ///       全項目が既定値のまま素通りし、個人状態と旧 BuildSettings.toml だけが拾われる。

    /// @note 個人状態を上書きで被せる。旧 editor_settings.toml に [camera] や [scene] が
    ///       残っていても local 側が勝つので、移行はこの 1 行で済む。
    OverlayEditorLocalState(projectRoot, tbl);

    /// @note カメラ
    if (auto v = tbl["camera"]["speed"].value<float>())        cameraSpeed        = *v;
    if (auto v = tbl["camera"]["sensitivity"].value<float>())  cameraSensitivity  = *v;
    if (auto v = tbl["camera"]["last_pos_x"].value<float>())   cameraLastPx       = *v;
    if (auto v = tbl["camera"]["last_pos_y"].value<float>())   cameraLastPy       = *v;
    if (auto v = tbl["camera"]["last_pos_z"].value<float>())   cameraLastPz       = *v;
    if (auto v = tbl["camera"]["last_rot_x"].value<float>())   cameraLastRx       = *v;
    if (auto v = tbl["camera"]["last_rot_y"].value<float>())   cameraLastRy       = *v;
    if (auto v = tbl["camera"]["last_rot_z"].value<float>())   cameraLastRz       = *v;
    if (auto v = tbl["camera"]["last_rot_w"].value<float>())   cameraLastRw       = *v;
    if (auto v = tbl["camera"]["orthographic"].value<bool>())  cameraOrthographic = *v;
    if (auto v = tbl["camera"]["ortho_height"].value<float>()) cameraOrthoHeight  = *v;

    /// @note ビュー
    if (auto v = tbl["view"]["show_grid"].value<bool>())      showGrid      = *v;
    if (auto v = tbl["view"]["grid_size"].value<float>())     gridSize      = *v;
    if (auto v = tbl["view"]["show_light_range"].value<bool>()) showLightRange = *v;
    if (auto v = tbl["view"]["show_vfx_gizmos"].value<bool>()) showVFXGizmos = *v;
    if (auto v = tbl["view"]["show_flow_fields"].value<bool>()) showFlowFields = *v;
    else                                                         showFlowFields = showVFXGizmos;
    if (auto v = tbl["view"]["show_flow_samples"].value<bool>())    showFlowSamples    = *v;
    if (auto v = tbl["view"]["show_physics_volumes"].value<bool>()) showPhysicsVolumes = *v;
    if (auto v = tbl["view"]["show_water_flow"].value<bool>())      showWaterFlow      = *v;
    if (auto v = tbl["view"]["show_ragdoll"].value<bool>()) showRagdoll = *v;
    if (auto v = tbl["view"]["show_skeleton"].value<bool>())  showSkeleton  = *v;
    if (auto v = tbl["view"]["skeleton_selected_only"].value<bool>()) skeletonSelectedOnly = *v;
    if (auto v = tbl["view"]["show_script_gizmos"].value<bool>())     showScriptGizmos     = *v;
    if (auto v = tbl["view"]["show_constraints"].value<bool>())       showConstraints      = *v;
    if (auto v = tbl["view"]["show_rigid_bodies"].value<bool>())      showRigidBodies      = *v;
    if (auto v = tbl["view"]["show_ik"].value<bool>())                showIK               = *v;
    if (auto v = tbl["view"]["show_spring_bones"].value<bool>())      showSpringBones      = *v;
    if (auto v = tbl["view"]["show_attachments"].value<bool>())       showAttachments      = *v;
    if (auto v = tbl["view"]["show_vfx_paths"].value<bool>())         showVFXPaths         = *v;
    if (auto v = tbl["view"]["show_terrain_bounds"].value<bool>())    showTerrainBounds    = *v;
    if (auto v = tbl["view"]["show_lod_bounds"].value<bool>())        showLODBounds        = *v;
    if (auto v = tbl["view"]["show_scene_icons"].value<bool>()) showSceneIcons = *v;
    hiddenSceneIcons.clear();
    if (auto* arr = tbl["view"]["hidden_scene_icons"].as_array()) {
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>()) hiddenSceneIcons.push_back(*v);
    }
    if (auto v = tbl["view"]["show_stats"].value<bool>())     showStats = *v;
    if (auto v = tbl["view"]["scene_view_occlusion_culling"].value<bool>())
        sceneViewOcclusionCulling = *v;
    if (auto v = tbl["view"]["surface_snap_align_to_normal"].value<bool>())
        surfaceSnapAlignToNormal = *v;

    /// @note スナップ
    if (auto v = tbl["snap"]["enabled"].value<bool>())   snapEnabled = *v;
    if (auto v = tbl["snap"]["pos"].value<float>())      snapPos     = *v;
    if (auto v = tbl["snap"]["rot"].value<float>())      snapRot     = *v;
    if (auto v = tbl["snap"]["scale"].value<float>())    snapScale   = *v;

    /// @note ギズモ
    if (auto v = tbl["gizmo"]["mode"].value<int64_t>())       gizmoMode     = static_cast<int>(*v);
    if (auto v = tbl["gizmo"]["space"].value<int64_t>())      gizmoSpace    = static_cast<int>(*v);
    if (auto v = tbl["gizmo"]["pivot"].value<int64_t>())      gizmoPivot    = static_cast<int>(*v);

    /// @note ゲームビュー
    if (auto v = tbl["game_view"]["aspect"].value<int64_t>()) gameViewportAspect = static_cast<int>(*v);
    if (auto v = tbl["game_view"]["play_focus_mode"].value<int64_t>()) playFocusMode = static_cast<int>(*v);

    /// @note その他
    if (auto v = tbl["misc"]["hot_reload"].value<bool>()) hotReloadEnabled = *v;
    if (auto v = tbl["misc"]["hot_reload_sound"].value<bool>()) hotReloadSound = *v;
    if (auto v = tbl["misc"]["ai_command_bus"].value<bool>()) aiCommandBusEnabled = *v;
    if (auto v = tbl["misc"]["developer_mode"].value<bool>()) developerMode = *v;
    if (auto v = tbl["misc"]["sweep_orphaned_baked"].value<bool>())
        sweepOrphanedBakedOnOpen = *v;

    /// @note ツールウィンドウ
    if (auto v = tbl["tools"]["show_terrain"].value<bool>()) showTerrainTool = *v;

    /// @note Map Mode フィルター
    if (auto v = tbl["map_mode"]["hierarchy_filter"].value<bool>()) mapHierarchyFilter = *v;
    if (auto v = tbl["map_mode"]["inspector_filter"].value<bool>()) mapInspectorFilter = *v;
    if (auto v = tbl["map_mode"]["active_tool"].value<int64_t>())   mapActiveTool = static_cast<int>(*v);

    /// @note Hierarchy
    if (auto v = tbl["hierarchy"]["show_generated_objects"].value<bool>()) showGeneratedObjects = *v;

    /// @note TerrainTool ブラシ設定
    if (auto v = tbl["terrain_tool"]["brush_radius"].value<float>())    terrainBrushRadius   = *v;
    if (auto v = tbl["terrain_tool"]["brush_strength"].value<float>())  terrainBrushStrength = *v;
    if (auto v = tbl["terrain_tool"]["brush_falloff"].value<int64_t>()) terrainBrushFalloff  = static_cast<int>(*v);
    if (auto v = tbl["terrain_tool"]["sculpt_mode"].value<int64_t>())   terrainSculptMode    = static_cast<int>(*v);
    if (auto v = tbl["terrain_tool"]["paint_layer"].value<int64_t>())   terrainPaintLayer    = static_cast<uint32_t>(*v);

    /// @note Camera Bookmarks
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

    /// @note デフォルトインポート設定
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

    /// @note ホットキーオーバーライド
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

    /// @note UI
    if (auto v = tbl["ui"]["language"].value<std::string>()) language = *v;
    if (auto v = tbl["ui"]["scale"].value<float>()) editorUiScale = *v;
    if (auto v = tbl["ui"]["multi_viewport"].value<bool>()) multiViewportEnabled = *v;

    /// @note 相対パスで保存されたパスを projectRoot と組み合わせて絶対パスへ戻すヘルパー。
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

    /// @note Asset Browser
    ///       1 パネルぶんの状態を 1 つのテーブルから読む。旧形式 ([asset_browser] 直下の
    ///       スカラー) も新形式 ([[asset_browser.panels]] の各要素) も同じ形なので共用する。
    const auto readPanelState = [](const auto& src, AssetBrowserPanelState& dst,
                                   const auto& toAbs) {
        if (auto v = src["icon_size"].template value<float>())    dst.iconSize  = *v;
        if (auto v = src["tree_width"].template value<float>())   dst.treeWidth = *v;
        if (auto v = src["view_mode"].template value<int64_t>())  dst.viewMode  = static_cast<int>(*v);
        if (auto v = src["sort_mode"].template value<int64_t>())  dst.sortMode  = static_cast<int>(*v);
        /// @note 新形式のマスクが無ければ、旧版が書いた単一 enum 値から組み立てる。名前を分けるのは if の条件変数のスコープが else 節まで伸びるため (同じ名前だと再定義になる)。
        if (auto mask = src["type_filter_mask"].template value<int64_t>())
            dst.typeFilterMask = static_cast<unsigned int>(*mask);
        else if (auto legacy = src["type_filter"].template value<int64_t>(); legacy && *legacy > 0)
            dst.typeFilterMask = 1u << static_cast<int>(*legacy);
        if (auto v = src["search_all_folders"].template value<bool>()) dst.searchAllFolders = *v;
        if (auto v = src["tree_show_files"].template value<bool>())    dst.treeShowFiles    = *v;
        if (auto v = src["current_folder"].template value<std::string>())
            dst.currentFolder = toAbs(*v);
    };

    assetBrowserPanels.clear();
    if (auto* arr = tbl["asset_browser"]["panels"].as_array()) {
        for (auto& elem : *arr) {
            AssetBrowserPanelState state;
            if (auto* entry = elem.as_table())
                readPanelState(*entry, state, toAbsProjectPath);
            assetBrowserPanels.push_back(std::move(state));
        }
    } else {
        /// @note 旧形式からの移行。1 枚目のパネルの状態として読み取る。
        AssetBrowserPanelState state;
        readPanelState(tbl["asset_browser"], state, toAbsProjectPath);
        assetBrowserPanels.push_back(std::move(state));
    }

    assetBrowserBookmarks.clear();
    if (auto* arr = tbl["asset_browser"]["bookmarks"].as_array()) {
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>()) assetBrowserBookmarks.push_back(*v);
    }
    assetBrowserFolderColors.clear();
    if (auto* arr = tbl["asset_browser"]["folder_colors"].as_array()) {
        for (auto& elem : *arr) {
            auto* entry = elem.as_table();
            if (!entry) continue;
            auto path  = (*entry)["path"].value<std::string>();
            auto color = (*entry)["color"].value<int64_t>();
            if (!path || !color) continue;
            assetBrowserFolderColors.emplace_back(toAbsProjectPath(*path),
                                                  static_cast<unsigned int>(*color));
        }
    }
    assetBrowserRecentFolderColors.clear();
    if (auto* arr = tbl["asset_browser"]["recent_folder_colors"].as_array()) {
        for (auto& elem : *arr)
            if (auto v = elem.value<int64_t>())
                assetBrowserRecentFolderColors.push_back(static_cast<unsigned int>(*v));
    }

    /// @note Console
    if (auto v = tbl["console"]["show_debug"].value<bool>())    consoleShowDebug   = *v;
    if (auto v = tbl["console"]["show_info"].value<bool>())     consoleShowInfo    = *v;
    if (auto v = tbl["console"]["show_warn"].value<bool>())     consoleShowWarn    = *v;
    if (auto v = tbl["console"]["show_error"].value<bool>())    consoleShowError   = *v;
    if (auto v = tbl["console"]["auto_scroll"].value<bool>())   consoleAutoScroll  = *v;
    if (auto v = tbl["console"]["collapse"].value<bool>())      consoleCollapse    = *v;
    if (auto v = tbl["console"]["clear_on_play"].value<bool>()) consoleClearOnPlay = *v;
    if (auto v = tbl["console"]["show_detail"].value<bool>())   consoleShowDetail  = *v;
    if (auto v = tbl["console"]["detail_ratio"].value<double>())
        consoleDetailRatio = std::clamp(static_cast<float>(*v), 0.10f, 0.80f);

    /// @note Animation Preview
    {
        const auto& ap = tbl["animation_preview"];
        if (auto v = ap["show_mesh"].value<bool>())        animPreviewShowMesh       = *v;
        if (auto v = ap["show_bones"].value<bool>())       animPreviewShowBones      = *v;
        if (auto v = ap["show_bone_names"].value<bool>())  animPreviewShowBoneNames  = *v;
        if (auto v = ap["show_trail"].value<bool>())       animPreviewShowTrail      = *v;
        if (auto v = ap["show_ghost"].value<bool>())       animPreviewShowGhost      = *v;
        if (auto v = ap["show_info"].value<bool>())        animPreviewShowInfo       = *v;
        if (auto v = ap["show_curves"].value<bool>())      animPreviewShowCurves     = *v;
        if (auto v = ap["show_root_motion"].value<bool>()) animPreviewShowRootMotion = *v;
        if (auto v = ap["label_mode"].value<int64_t>())
            animPreviewLabelMode = std::clamp(static_cast<int>(*v), 0, 2);
        if (auto v = ap["ghost_offset"].value<double>())
            animPreviewGhostOffset = std::clamp(static_cast<float>(*v), 0.0f, 0.5f);
        if (auto v = ap["loop"].value<bool>())             animPreviewLoop           = *v;
        if (auto v = ap["speed"].value<double>())
            animPreviewSpeed = std::clamp(static_cast<float>(*v), 0.05f, 4.0f);
        if (auto v = ap["camera_yaw"].value<double>())
            animPreviewCameraYaw = static_cast<float>(*v);
        if (auto v = ap["camera_pitch"].value<double>())
            animPreviewCameraPitch = std::clamp(static_cast<float>(*v), -1.35f, 1.35f);
        if (auto v = ap["show_grid"].value<bool>())        animPreviewShowGrid       = *v;
        if (auto v = ap["show_ground_ring"].value<bool>()) animPreviewShowGroundRing = *v;
        if (auto v = ap["wireframe"].value<bool>())        animPreviewWireframe      = *v;
        if (auto v = ap["show_axis_gizmo"].value<bool>())  animPreviewShowAxisGizmo  = *v;
        if (auto v = ap["background"].value<int64_t>())
            animPreviewBackground = std::clamp(static_cast<int>(*v), 0, 3);
        if (auto v = ap["fov"].value<double>())
            animPreviewFov = std::clamp(static_cast<float>(*v), 12.0f, 90.0f);
        if (auto v = ap["light_yaw"].value<double>())
            animPreviewLightYaw = static_cast<float>(*v);
    }

    /// @note パネル表示状態
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

    /// @note Debug メニュー - レンダリングオーバーレイ
    if (auto v = tbl["debug"]["show_colliders"].value<bool>())         showColliders        = *v;
    if (auto v = tbl["debug"]["show_ui_rects"].value<bool>())          showUIRects          = *v;
    if (auto v = tbl["debug"]["show_terrain_collision"].value<bool>()) showTerrainCollision = *v;
    if (auto v = tbl["debug"]["show_decal_bounds"].value<bool>())      showDecalBounds      = *v;
    if (auto v = tbl["debug"]["show_navmesh"].value<bool>())           showNavMesh          = *v;
    if (auto v = tbl["debug"]["show_nav_sensors"].value<bool>())       showNavSensors       = *v;
    if (auto v = tbl["debug"]["navmesh_draw_mode"].value<int64_t>())   navMeshDrawMode      = static_cast<int>(*v);
    if (auto v = tbl["debug"]["navmesh_draw_distance"].value<double>()) navMeshDrawDistance = static_cast<float>(*v);
    if (auto v = tbl["debug"]["view_mode"].value<int64_t>())      viewMode = static_cast<int>(*v);

    /// @note Inspector セクション折り畳み状態
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

    /// @note シーン
    if (auto v = tbl["scene"]["last_path"].value<std::string>())
        lastScenePath = toAbsProjectPath(*v);

    /// @note 最近開いたシーン
    recentScenes.clear();
    if (auto* arr = tbl["scene"]["recent"].as_array()) {
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>())
                recentScenes.push_back(toAbsProjectPath(*v));
    }

    /// @note オートセーブ
    if (auto v = tbl["autosave"]["enabled"].value<bool>())       autoSaveEnabled     = *v;
    if (auto v = tbl["autosave"]["interval_sec"].value<int64_t>()) autoSaveIntervalSec = static_cast<int>(*v);
    if (auto v = tbl["autosave"]["fluid_editor"].value<bool>())   fluidEditorAutoSave = *v;
    if (auto v = tbl["fluid_editor"]["follow_selection"].value<bool>()) fluidEditorFollowSelection = *v;

    /// @note 最近 Fluid Editor で開いた .fluid
    recentFluids.clear();
    /// @note 節を分ける: 履歴は個人の作業状態なので EditorLocalState.toml 側に置く。`OverlayEditorLocalState` はセクション単位で差し替えるため、
    ///       同じ [fluid_editor] を両方のファイルに置くと local 側が共有側の follow_selection ごと隠してしまう。
    if (auto* arr = tbl["fluid_editor_state"]["recent"].as_array()) {
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>()) recentFluids.push_back(toAbsProjectPath(*v));
    }
    /// @note 手で書き足された / 古い形式で膨らんだファイルからでも件数は超えて持たない。
    if (recentFluids.size() > static_cast<std::size_t>(kMaxRecentFluids))
        recentFluids.resize(static_cast<std::size_t>(kMaxRecentFluids));

    /// @note Build Settings
    if (auto* buildTbl = tbl["build"].as_table()) {
        build.productName       = (*buildTbl)["product_name"].value_or(build.productName);
        build.version           = (*buildTbl)["version"].value_or(build.version);
        build.outputDirectory   = (*buildTbl)["output_dir"].value_or(build.outputDirectory);
        build.iconPath          = (*buildTbl)["icon"].value_or(build.iconPath);
        build.developmentBuild  = (*buildTbl)["development"].value_or(build.developmentBuild);
        build.stripEditorAssets = (*buildTbl)["strip_editor_assets"].value_or(build.stripEditorAssets);

        build.scenes.clear();
        if (auto* scenesArr = (*buildTbl)["scenes"].as_array()) {
            for (auto& elem : *scenesArr) {
                auto* entryTbl = elem.as_table();
                if (!entryTbl) continue;
                SceneEntry entry;
                entry.path    = (*entryTbl)["path"].value_or(std::string{});
                entry.enabled = (*entryTbl)["enabled"].value_or(true);
                if (!entry.path.empty()) build.scenes.push_back(std::move(entry));
            }
        }
    } else {
        /// @note 旧 `<projectRoot>/BuildSettings.toml` を一度だけ引き継ぐ。
        BuildSettings::LoadLegacyFile(projectRoot, build);
    }

    return sharedLoaded;
}

bool EditorSettings::Save(const std::string& path, const std::string& projectRoot) const
{
    /// @note カメラ。これだけ書き出し先が Library/EditorLocalState.toml (git 管理外)。
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
    camTbl.insert("orthographic", cameraOrthographic);
    camTbl.insert("ortho_height", cameraOrthoHeight);

    /// @note ビュー
    toml::table viewTbl;
    viewTbl.insert("show_grid",        showGrid);
    viewTbl.insert("grid_size",        gridSize);
    viewTbl.insert("show_light_range", showLightRange);
    viewTbl.insert("show_vfx_gizmos", showVFXGizmos);
    viewTbl.insert("show_flow_fields",     showFlowFields);
    viewTbl.insert("show_flow_samples",    showFlowSamples);
    viewTbl.insert("show_physics_volumes", showPhysicsVolumes);
    viewTbl.insert("show_water_flow",      showWaterFlow);
    viewTbl.insert("show_ragdoll", showRagdoll);
    viewTbl.insert("show_skeleton",    showSkeleton);
    viewTbl.insert("skeleton_selected_only", skeletonSelectedOnly);
    viewTbl.insert("show_script_gizmos",     showScriptGizmos);
    viewTbl.insert("show_constraints",       showConstraints);
    viewTbl.insert("show_rigid_bodies",      showRigidBodies);
    viewTbl.insert("show_ik",                showIK);
    viewTbl.insert("show_spring_bones",      showSpringBones);
    viewTbl.insert("show_attachments",       showAttachments);
    viewTbl.insert("show_vfx_paths",         showVFXPaths);
    viewTbl.insert("show_terrain_bounds",    showTerrainBounds);
    viewTbl.insert("show_lod_bounds",        showLODBounds);
    viewTbl.insert("show_scene_icons", showSceneIcons);
    {
        toml::array hiddenArr;
        for (const auto& key : hiddenSceneIcons) hiddenArr.push_back(key);
        viewTbl.insert("hidden_scene_icons", std::move(hiddenArr));
    }
    viewTbl.insert("show_stats",       showStats);
    viewTbl.insert("scene_view_occlusion_culling", sceneViewOcclusionCulling);
    viewTbl.insert("surface_snap_align_to_normal", surfaceSnapAlignToNormal);

    /// @note スナップ
    toml::table snapTbl;
    snapTbl.insert("enabled", snapEnabled);
    snapTbl.insert("pos",     snapPos);
    snapTbl.insert("rot",     snapRot);
    snapTbl.insert("scale",   snapScale);

    /// @note ギズモ
    toml::table gizmoTbl;
    gizmoTbl.insert("mode",  static_cast<int64_t>(gizmoMode));
    gizmoTbl.insert("space", static_cast<int64_t>(gizmoSpace));
    gizmoTbl.insert("pivot", static_cast<int64_t>(gizmoPivot));

    /// @note ゲームビュー
    toml::table gameViewTbl;
    gameViewTbl.insert("aspect", static_cast<int64_t>(gameViewportAspect));
    gameViewTbl.insert("play_focus_mode", static_cast<int64_t>(playFocusMode));

    /// @note その他
    toml::table miscTbl;
    miscTbl.insert("hot_reload", hotReloadEnabled);
    miscTbl.insert("hot_reload_sound", hotReloadSound);
    miscTbl.insert("ai_command_bus", aiCommandBusEnabled);
    miscTbl.insert("developer_mode", developerMode);
    miscTbl.insert("sweep_orphaned_baked", sweepOrphanedBakedOnOpen);

    /// @note ツールウィンドウ
    toml::table toolsTbl;
    toolsTbl.insert("show_terrain", showTerrainTool);

    /// @note Map Mode フィルター
    toml::table mapModeTbl;
    mapModeTbl.insert("hierarchy_filter", mapHierarchyFilter);
    mapModeTbl.insert("inspector_filter", mapInspectorFilter);
    mapModeTbl.insert("active_tool",      static_cast<int64_t>(mapActiveTool));

    /// @note Hierarchy
    toml::table hierarchyTbl;
    hierarchyTbl.insert("show_generated_objects", showGeneratedObjects);

    /// @note TerrainTool ブラシ設定
    toml::table terrainToolTbl;
    terrainToolTbl.insert("brush_radius",   terrainBrushRadius);
    terrainToolTbl.insert("brush_strength", terrainBrushStrength);
    terrainToolTbl.insert("brush_falloff",  static_cast<int64_t>(terrainBrushFalloff));
    terrainToolTbl.insert("sculpt_mode",    static_cast<int64_t>(terrainSculptMode));
    terrainToolTbl.insert("paint_layer",    static_cast<int64_t>(terrainPaintLayer));

    /// @note Camera Bookmarks
    toml::array camBkArr;
    for (const auto& bk : cameraBookmarks) {
        toml::table t;
        t.insert("valid", bk.valid);
        t.insert("px", bk.px); t.insert("py", bk.py); t.insert("pz", bk.pz);
        t.insert("rx", bk.rx); t.insert("ry", bk.ry);
        t.insert("rz", bk.rz); t.insert("rw", bk.rw);
        camBkArr.push_back(std::move(t));
    }

    /// @note 絶対パスを projectRoot 相対へ変換して保存する (プロジェクトを移動しても壊れない)。
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

    /// @note Asset Browser
    toml::table assetBrowserTbl;
    {
        toml::array panelArr;
        for (const AssetBrowserPanelState& state : assetBrowserPanels) {
            toml::table entry;
            entry.insert("icon_size",          state.iconSize);
            entry.insert("tree_width",         state.treeWidth);
            entry.insert("view_mode",          static_cast<int64_t>(state.viewMode));
            entry.insert("sort_mode",          static_cast<int64_t>(state.sortMode));
            entry.insert("type_filter_mask",   static_cast<int64_t>(state.typeFilterMask));
            entry.insert("search_all_folders", state.searchAllFolders);
            entry.insert("tree_show_files",    state.treeShowFiles);
            entry.insert("current_folder",     toRelProjectPath(state.currentFolder));
            panelArr.push_back(std::move(entry));
        }
        assetBrowserTbl.insert("panels", std::move(panelArr));
    }
    {
        toml::array bkArr;
        for (const auto& bk : assetBrowserBookmarks) bkArr.push_back(bk);
        assetBrowserTbl.insert("bookmarks", std::move(bkArr));
    }
    {
        toml::array colorArr;
        for (const auto& [path, color] : assetBrowserFolderColors) {
            toml::table entry;
            entry.insert("path",  toRelProjectPath(path));
            entry.insert("color", static_cast<int64_t>(color));
            colorArr.push_back(std::move(entry));
        }
        assetBrowserTbl.insert("folder_colors", std::move(colorArr));
    }
    {
        toml::array recentArr;
        for (unsigned int color : assetBrowserRecentFolderColors)
            recentArr.push_back(static_cast<int64_t>(color));
        assetBrowserTbl.insert("recent_folder_colors", std::move(recentArr));
    }

    /// @note Console
    toml::table consoleTbl;
    consoleTbl.insert("show_debug",    consoleShowDebug);
    consoleTbl.insert("show_info",     consoleShowInfo);
    consoleTbl.insert("show_warn",     consoleShowWarn);
    consoleTbl.insert("show_error",    consoleShowError);
    consoleTbl.insert("auto_scroll",   consoleAutoScroll);
    consoleTbl.insert("collapse",      consoleCollapse);
    consoleTbl.insert("clear_on_play", consoleClearOnPlay);
    consoleTbl.insert("show_detail",   consoleShowDetail);
    consoleTbl.insert("detail_ratio",  static_cast<double>(consoleDetailRatio));

    /// @note Animation Preview
    toml::table animPreviewTbl;
    animPreviewTbl.insert("show_mesh",        animPreviewShowMesh);
    animPreviewTbl.insert("show_bones",       animPreviewShowBones);
    animPreviewTbl.insert("show_bone_names",  animPreviewShowBoneNames);
    animPreviewTbl.insert("show_trail",       animPreviewShowTrail);
    animPreviewTbl.insert("show_ghost",       animPreviewShowGhost);
    animPreviewTbl.insert("show_info",        animPreviewShowInfo);
    animPreviewTbl.insert("show_curves",      animPreviewShowCurves);
    animPreviewTbl.insert("show_root_motion", animPreviewShowRootMotion);
    animPreviewTbl.insert("label_mode",       static_cast<int64_t>(animPreviewLabelMode));
    animPreviewTbl.insert("ghost_offset",     static_cast<double>(animPreviewGhostOffset));
    animPreviewTbl.insert("loop",             animPreviewLoop);
    animPreviewTbl.insert("speed",            static_cast<double>(animPreviewSpeed));
    animPreviewTbl.insert("camera_yaw",       static_cast<double>(animPreviewCameraYaw));
    animPreviewTbl.insert("camera_pitch",     static_cast<double>(animPreviewCameraPitch));
    animPreviewTbl.insert("show_grid",        animPreviewShowGrid);
    animPreviewTbl.insert("show_ground_ring", animPreviewShowGroundRing);
    animPreviewTbl.insert("wireframe",        animPreviewWireframe);
    animPreviewTbl.insert("show_axis_gizmo",  animPreviewShowAxisGizmo);
    animPreviewTbl.insert("background",       static_cast<int64_t>(animPreviewBackground));
    animPreviewTbl.insert("fov",              static_cast<double>(animPreviewFov));
    animPreviewTbl.insert("light_yaw",        static_cast<double>(animPreviewLightYaw));

    /// @note パネル表示状態
    toml::array panelArr;
    for (const auto& [name, open] : panelVisibility) {
        toml::table entry;
        entry.insert("name", name);
        entry.insert("open", open);
        panelArr.push_back(std::move(entry));
    }
    toml::table panelsTbl;
    panelsTbl.insert("visible", std::move(panelArr));

    /// @note Inspector セクション折り畳み状態
    toml::array statesArr;
    for (auto& [key, open] : inspectorSectionState) {
        toml::table entry;
        entry.insert("k", static_cast<int64_t>(key));
        entry.insert("v", open);
        statesArr.push_back(std::move(entry));
    }
    toml::table inspectorTbl;
    inspectorTbl.insert("states", std::move(statesArr));

    /// @note シーン
    toml::table sceneTbl;
    sceneTbl.insert("last_path", toRelProjectPath(lastScenePath));
    {
        toml::array recentArr;
        for (const auto& s : recentScenes) recentArr.push_back(toRelProjectPath(s));
        sceneTbl.insert("recent", std::move(recentArr));
    }

    /// @note オートセーブ
    toml::table autosaveTbl;
    autosaveTbl.insert("enabled",      autoSaveEnabled);
    autosaveTbl.insert("interval_sec", static_cast<int64_t>(autoSaveIntervalSec));
    autosaveTbl.insert("fluid_editor",  fluidEditorAutoSave);

    toml::table fluidEditorTbl;
    fluidEditorTbl.insert("follow_selection", fluidEditorFollowSelection);

    /// @note 履歴は個人の作業状態。共有ファイルへ置くと、同じプロジェクトを触る全員がこの行で衝突する
    ///       (recentScenes が [scene] で local 側に居るのと同じ理由)。
    toml::table fluidEditorStateTbl;
    {
        toml::array recentArr;
        for (const auto& p : recentFluids) recentArr.push_back(toRelProjectPath(p));
        fluidEditorStateTbl.insert("recent", std::move(recentArr));
    }

    /// @note Debug メニュー - レンダリングオーバーレイ
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

    /// @note ホットキーオーバーライド
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

    /// @note デフォルトインポート設定
    toml::table importTbl;
    importTbl.insert("source_dcc",                static_cast<int64_t>(defaultImportOptions.sourceDcc));
    importTbl.insert("up_axis",                   static_cast<int64_t>(defaultImportOptions.upAxis));
    importTbl.insert("normal_map_convention",    static_cast<int64_t>(defaultImportOptions.normalMapConvention));
    importTbl.insert("unit_scale_multiplier",    defaultImportOptions.unitScaleMultiplier);
    importTbl.insert("generate_normals",         defaultImportOptions.generateNormals);
    importTbl.insert("generate_tangents",        defaultImportOptions.generateTangents);
    importTbl.insert("generate_tex_descriptors", defaultImportOptions.generateTexDescriptors);
    importTbl.insert("default_compression",      static_cast<int64_t>(defaultImportOptions.defaultCompression));

    /// @note Build Settings
    toml::table buildTbl;
    buildTbl.insert("product_name",        build.productName);
    buildTbl.insert("version",             build.version);
    buildTbl.insert("output_dir",          build.outputDirectory);
    buildTbl.insert("icon",                build.iconPath);
    buildTbl.insert("development",         build.developmentBuild);
    buildTbl.insert("strip_editor_assets", build.stripEditorAssets);
    {
        toml::array scenesArr;
        for (const auto& scene : build.scenes) {
            toml::table entry;
            entry.insert("path",    scene.path);
            entry.insert("enabled", scene.enabled);
            scenesArr.push_back(std::move(entry));
        }
        buildTbl.insert("scenes", std::move(scenesArr));
    }

    /// @note UI スケール
    toml::table uiTbl;
    uiTbl.insert("language", language);
    uiTbl.insert("scale", editorUiScale);
    uiTbl.insert("multi_viewport", multiViewportEnabled);

    /// @name 個人の作業状態 (Library/EditorLocalState.toml)
    /// @note 「その人がどこで何を開いて作業していたか」しか入っていないセクション。
    ///       共有しても相手の役に立たず、触るたびに書き換わるので分けて置く。
    toml::table localRoot;
    localRoot.insert("camera",             std::move(camTbl));
    localRoot.insert("camera_bookmarks",   std::move(camBkArr));
    localRoot.insert("scene",              std::move(sceneTbl));
    localRoot.insert("asset_browser",      std::move(assetBrowserTbl));
    localRoot.insert("panels",             std::move(panelsTbl));
    localRoot.insert("console",            std::move(consoleTbl));
    localRoot.insert("animation_preview",  std::move(animPreviewTbl));
    localRoot.insert("inspector_sections", std::move(inspectorTbl));
    localRoot.insert("fluid_editor_state", std::move(fluidEditorStateTbl));

    /// @note 個人状態は共有ファイルより先に片付ける。ここが失敗しても共有側の保存は続ける
    ///       (作業状態を落とすだけで、ビルド設定やホットキーまで巻き添えにする理由が無い)。
    (void)SaveEditorLocalState(projectRoot, std::move(localRoot));

    /// @name 共有設定 (Assets/EditorConfig/editor_settings.toml)
    toml::table root;
    root.insert("ui",                 std::move(uiTbl));
    root.insert("view",               std::move(viewTbl));
    root.insert("snap",               std::move(snapTbl));
    root.insert("gizmo",              std::move(gizmoTbl));
    root.insert("game_view",          std::move(gameViewTbl));
    root.insert("misc",               std::move(miscTbl));
    root.insert("tools",              std::move(toolsTbl));
    root.insert("map_mode",           std::move(mapModeTbl));
    root.insert("hierarchy",          std::move(hierarchyTbl));
    root.insert("terrain_tool",       std::move(terrainToolTbl));
    root.insert("hotkeys",            std::move(hkTbl));
    root.insert("import",             std::move(importTbl));
    root.insert("autosave",           std::move(autosaveTbl));
    root.insert("fluid_editor",       std::move(fluidEditorTbl));
    root.insert("debug",              std::move(debugTbl));
    root.insert("build",              std::move(buildTbl));

    std::ostringstream ss;
    ss << root;
    return util::FileSystem::WriteText(path, ss.str());
}

} // namespace fbzz::editor
