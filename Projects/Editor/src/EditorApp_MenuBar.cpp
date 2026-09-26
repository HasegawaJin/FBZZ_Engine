/// @file    EditorApp_MenuBar.cpp
/// @brief   メインメニューバーの構築とホットキー登録。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// @note UI 記述に特化した独立ファイル。ライフサイクル管理やシーン I/O と関心を分離する。
#include <Editor/EditorApp.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Panels/AiSettingsPanel.hpp>
#include <Editor/Panels/AssetMaintenancePanel.hpp>
#include <Editor/Panels/IblBakePanel.hpp>
#include <Editor/Panels/VolumeFlipbookBakePanel.hpp>
#include <Editor/Panels/NavigationPanel.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/CreateObjectMenu.hpp>
#include <Editor/Util/HotkeyManager.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/Localization.hpp>
#include <Editor/Util/ObjectPresets.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
#include <Editor/Util/Toast.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Scene/ScriptValidation.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Cursor.hpp>
#include <Engine/Core/DeveloperMode.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Audio/AudioManager.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace fbzz::editor {

namespace {

enum class PlayToolbarIcon {
    Play,
    Stop,
    Pause,
    Step,
    Reload,
    /// @brief コンパイル / リロード中。↻ を回し続ける。
    ReloadBusy,
    /// @brief 直近のスクリプトリロードが成功。
    ReloadOk,
    /// @brief 直近のスクリプトリロードが失敗。
    ReloadFailed
};

void DrawPlayToolbarIcon(PlayToolbarIcon icon, const ImVec2& min, const ImVec2& max, ImU32 color)
{
    /// @note フォント非依存の固定アイコンにするため、ImGui の DrawList で幾何形状を直接描く。
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImDrawListFlags oldFlags = drawList->Flags;
    drawList->Flags |= ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AntiAliasedLines;

    auto drawTriangle = [drawList, color](const ImVec2& a, const ImVec2& b, const ImVec2& c) {
        /// @note 塗りつぶし三角形だけだと斜辺のジャギーが目立つため、同色の線を輪郭に重ねる。
        drawList->AddTriangleFilled(a, b, c, color);
        drawList->AddTriangle(a, b, c, color, 1.35f);
    };

    const ImVec2 center { (min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f };

    switch (icon) {
    case PlayToolbarIcon::Play:
        drawTriangle(
            { center.x - 4.0f, center.y - 7.0f },
            { center.x - 4.0f, center.y + 7.0f },
            { center.x + 7.0f, center.y });
        break;
    case PlayToolbarIcon::Stop:
        drawList->AddRectFilled(
            { center.x - 5.5f, center.y - 5.5f },
            { center.x + 5.5f, center.y + 5.5f },
            color,
            1.5f);
        break;
    case PlayToolbarIcon::Pause:
        drawList->AddRectFilled(
            { center.x - 6.0f, center.y - 7.0f },
            { center.x - 2.0f, center.y + 7.0f },
            color,
            1.0f);
        drawList->AddRectFilled(
            { center.x + 2.0f, center.y - 7.0f },
            { center.x + 6.0f, center.y + 7.0f },
            color,
            1.0f);
        break;
    case PlayToolbarIcon::Step:
        drawTriangle(
            { center.x - 7.0f, center.y - 6.5f },
            { center.x - 7.0f, center.y + 6.5f },
            { center.x + 2.0f, center.y });
        drawList->AddRectFilled(
            { center.x + 5.0f, center.y - 7.0f },
            { center.x + 7.0f, center.y + 7.0f },
            color,
            1.0f);
        break;
    case PlayToolbarIcon::ReloadOk:
        drawList->AddPolyline(
            std::array<ImVec2, 3>{ ImVec2{ center.x - 6.0f, center.y + 0.5f },
                                   ImVec2{ center.x - 2.0f, center.y + 4.5f },
                                   ImVec2{ center.x + 6.5f, center.y - 5.0f } }.data(),
            3, color, 2.2f);
        break;
    case PlayToolbarIcon::ReloadFailed:
        drawList->AddLine({ center.x - 5.0f, center.y - 5.0f }, { center.x + 5.0f, center.y + 5.0f }, color, 2.2f);
        drawList->AddLine({ center.x + 5.0f, center.y - 5.0f }, { center.x - 5.0f, center.y + 5.0f }, color, 2.2f);
        break;
    case PlayToolbarIcon::Reload:
    case PlayToolbarIcon::ReloadBusy: {
        /// @note 円弧 (約 300°) + 先端に矢頭
        constexpr float kPi        = 3.14159265f;
        constexpr float r          = 5.5f;
        /// @note Busy は 1 周 1 秒で回す。
        const float spin           = icon == PlayToolbarIcon::ReloadBusy
            ? static_cast<float>(std::fmod(ImGui::GetTime(), 1.0)) * kPi * 2.0f : 0.0f;
        const float startAngle     = kPi * 0.25f + spin;
        const float endAngle       = startAngle + kPi * 1.67f;
        drawList->PathArcTo(center, r, startAngle, endAngle, 16);
        drawList->PathStroke(color, 1.8f);

        const float ax = center.x + r * std::cos(endAngle);
        const float ay = center.y + r * std::sin(endAngle);
        const float tx = endAngle + kPi * 0.5f;
        constexpr float arrowSize = 3.5f;
        drawList->AddTriangleFilled(
            { ax, ay },
            { ax + arrowSize * std::cos(tx - 0.55f), ay + arrowSize * std::sin(tx - 0.55f) },
            { ax + arrowSize * std::cos(tx + 0.55f), ay + arrowSize * std::sin(tx + 0.55f) },
            color);
        break;
    }
    }

    drawList->Flags = oldFlags;
}

ImVec4 WithAlpha(ImVec4 color, float alpha)
{
    color.w = alpha;
    return color;
}

bool PlayToolbarButton(
    const char* id,
    const char* tooltip,
    PlayToolbarIcon icon,
    bool enabled,
    bool active,
    const ImVec4& activeColor,
    const ImVec2& size,
    const ImVec4* iconColorOverride = nullptr)
{
    /// @note Play 操作は常に同じ位置に置き、状態確認と操作を視線移動なしで行えるようにする。
    ///       active 時は操作種別の色を背景へ乗せ、無効時は Disabled スタイルで入力も止める。
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, activeColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(activeColor, 0.92f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithAlpha(activeColor, 1.0f));
    }

    if (!enabled)
        ImGui::BeginDisabled();

    const bool pressed = ImGui::Button(id, size);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImU32 iconColor = iconColorOverride
        ? ImGui::ColorConvertFloat4ToU32(*iconColorOverride)
        : ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    DrawPlayToolbarIcon(icon, min, max, iconColor);

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);

    if (!enabled)
        ImGui::EndDisabled();

    if (active)
        ImGui::PopStyleColor(3);

    return enabled && pressed;
}

}

void EditorApp::InstallNativeMenuBar()
{
    using NativeMenuItem = core::Window::NativeMenuItem;
    using MenuList = std::vector<NativeMenuItem>;

    constexpr uint16_t NEW_SCENE       = 100;
    constexpr uint16_t OPEN_SCENE      = 101;
    constexpr uint16_t SAVE_SCENE      = 102;
    constexpr uint16_t SAVE_SCENE_AS   = 103;
    constexpr uint16_t CLOSE_PREFAB    = 104;
    constexpr uint16_t SAVE_ALL_ASSETS = 105;
    constexpr uint16_t EXIT_EDITOR     = 106;
    constexpr uint16_t UNDO            = 200;
    constexpr uint16_t REDO            = 201;
    constexpr uint16_t PLAY            = 300;
    constexpr uint16_t STOP            = 301;
    constexpr uint16_t PAUSE           = 302;
    constexpr uint16_t STEP            = 303;
    constexpr uint16_t RELOAD_SCRIPTS  = 304;
    constexpr uint16_t OPEN_ANALYSIS   = 400;
    constexpr uint16_t TOGGLE_PASS_VIEWER = 423;
    constexpr uint16_t TOGGLE_GRID     = 401;
    constexpr uint16_t TOGGLE_LIGHTS   = 402;
    constexpr uint16_t TOGGLE_VFX      = 403;
    constexpr uint16_t TOGGLE_SKELETON = 404;
    constexpr uint16_t TOGGLE_STATS    = 405;
    constexpr uint16_t TOGGLE_HOTRELOAD = 406;
    constexpr uint16_t TOGGLE_HOTRELOAD_SOUND = 439;
    constexpr uint16_t TOGGLE_COLLIDERS = 407;
    constexpr uint16_t TOGGLE_TERRAIN_COLLISION = 408;
    constexpr uint16_t TOGGLE_NAVMESH = 409;
    constexpr uint16_t TOGGLE_AI_SENSORS = 414;
    constexpr uint16_t TOGGLE_DECAL_BOUNDS = 415;
    constexpr uint16_t TOGGLE_SHADOW = 422;
    constexpr uint16_t TOGGLE_SCENE_ICONS     = 424;
    constexpr uint16_t TOGGLE_SCRIPT_GIZMOS   = 425;
    constexpr uint16_t TOGGLE_SKELETON_SEL    = 426;
    constexpr uint16_t TOGGLE_CONSTRAINTS     = 427;
    constexpr uint16_t TOGGLE_RIGID_BODIES    = 428;
    constexpr uint16_t TOGGLE_IK              = 429;
    constexpr uint16_t TOGGLE_SPRING_BONES    = 430;
    constexpr uint16_t TOGGLE_ATTACHMENTS     = 431;
    constexpr uint16_t TOGGLE_VFX_PATHS       = 432;
    constexpr uint16_t TOGGLE_TERRAIN_BOUNDS  = 433;
    constexpr uint16_t TOGGLE_LOD_BOUNDS      = 434;
    constexpr uint16_t TOGGLE_FLOW_FIELDS     = 435;
    constexpr uint16_t TOGGLE_FLOW_SAMPLES    = 436;
    constexpr uint16_t TOGGLE_PHYSICS_VOLUMES = 437;
    constexpr uint16_t TOGGLE_WATER_FLOW      = 438;
    constexpr uint16_t VIEW_LIT        = 410;
    constexpr uint16_t VIEW_UNLIT      = 411;
    constexpr uint16_t VIEW_WIRE_LIT   = 412;
    constexpr uint16_t VIEW_WIRE_UNLIT = 413;
    constexpr uint16_t TOGGLE_MAP       = 501;
    constexpr uint16_t OPEN_BUILD       = 502;
    constexpr uint16_t OPEN_IBL         = 503;
    constexpr uint16_t OPEN_NAVIGATION  = 504;
    constexpr uint16_t OPEN_ASSET_MAINT = 505;
    constexpr uint16_t OPEN_VOLUME_FLIPBOOK = 506;
    constexpr uint16_t OPEN_AI_SETTINGS = 600;
    constexpr uint16_t PANEL_BASE       = 1000;
    constexpr uint16_t CREATE_EMPTY     = 2999;
    constexpr uint16_t PRESET_BASE      = 3000;

    const auto command = [](const char* label, uint16_t id) {
        return NativeMenuItem{ util::StringUtils::ToWide(label), id, false, {} };
    };
    const auto separator = []() { return NativeMenuItem{ {}, 0, true, {} }; };
    const auto submenu = [](const char* label, MenuList children) {
        return NativeMenuItem{ util::StringUtils::ToWide(label), 0, false, std::move(children) };
    };

    MenuList panels;
    for (std::size_t i = 0; i < m_panels.size(); ++i) {
        if (!m_panels[i]->ShowInViewMenu()) continue;
        panels.push_back(command(m_panels[i]->GetViewMenuName(),
                                 static_cast<uint16_t>(PANEL_BASE + i)));
    }

    MenuList debugViewMode;
    debugViewMode.push_back(command("Lit", VIEW_LIT));
    debugViewMode.push_back(command("Unlit", VIEW_UNLIT));
    debugViewMode.push_back(command("Wireframe Lit", VIEW_WIRE_LIT));
    debugViewMode.push_back(command("Wireframe Unlit", VIEW_WIRE_UNLIT));

    MenuList terrainTools;
    terrainTools.push_back(command("Terrain Tool", 510));

    /// @note プリセットは Create Empty と同じくルートの注視点へ置く。ネイティブメニューは選択を親にする文脈を持たない。
    const auto presets = ObjectPresetCatalog();
    MenuList gameObject;
    gameObject.push_back(command("Create Empty", CREATE_EMPTY));
    gameObject.push_back(separator());
    for (std::size_t i = 0; i < presets.size(); ++i) {
        const ObjectPreset& preset = presets[i];
        const std::string label(preset.label);
        const uint16_t presetId = static_cast<uint16_t>(PRESET_BASE + i);
        if (preset.category.empty()) {
            gameObject.push_back(command(label.c_str(), presetId));
            continue;
        }
        const std::wstring categoryName = util::StringUtils::ToWide(std::string(preset.category));
        if (gameObject.back().label != categoryName)
            gameObject.push_back(submenu(std::string(preset.category).c_str(), {}));
        gameObject.back().children.push_back(command(label.c_str(), presetId));
    }

    MenuList menus;
    menus.push_back(submenu("File", {
        command("New Scene", NEW_SCENE),
        command("Open...", OPEN_SCENE),
        command("Save", SAVE_SCENE),
        command("Save As...", SAVE_SCENE_AS),
        command("Close Prefab", CLOSE_PREFAB),
        command("Save All Assets", SAVE_ALL_ASSETS),
        separator(),
        command("Exit", EXIT_EDITOR)
    }));
    menus.push_back(submenu("Edit", {
        command("Undo", UNDO), command("Redo", REDO)
    }));
    menus.push_back(submenu("GameObject", std::move(gameObject)));
    menus.push_back(submenu("Play", {
        command("Play", PLAY), command("Stop", STOP), command("Pause", PAUSE),
        command("Step", STEP), separator(), command("Reload Scripts", RELOAD_SCRIPTS)
    }));
    menus.push_back(submenu("View", {
        submenu("Panels", std::move(panels)),
        command("Reset UI Scale", 700)
    }));
    menus.push_back(submenu("Debug", {
        command("Analysis", OPEN_ANALYSIS), command("Render Pass Viewer", TOGGLE_PASS_VIEWER), separator(),
        command("Grid", TOGGLE_GRID), command("Light Range", TOGGLE_LIGHTS),
        command("VFX Emitters", TOGGLE_VFX),
        command("Flow Fields", TOGGLE_FLOW_FIELDS), command("Flow Samples", TOGGLE_FLOW_SAMPLES),
        command("Skeleton", TOGGLE_SKELETON), command("Skeleton: Selected Only", TOGGLE_SKELETON_SEL),
        command("Stats", TOGGLE_STATS),
        command("Scene Icons", TOGGLE_SCENE_ICONS), command("Script Gizmos", TOGGLE_SCRIPT_GIZMOS),
        command("IK Chains", TOGGLE_IK), command("Spring Bones", TOGGLE_SPRING_BONES),
        command("Attachments", TOGGLE_ATTACHMENTS), command("VFX Paths", TOGGLE_VFX_PATHS),
        separator(),
        command("Colliders", TOGGLE_COLLIDERS), command("Constraints", TOGGLE_CONSTRAINTS),
        command("Rigid Bodies", TOGGLE_RIGID_BODIES), command("Physics Volumes", TOGGLE_PHYSICS_VOLUMES),
        command("Water Flow", TOGGLE_WATER_FLOW),
        command("Terrain Collision", TOGGLE_TERRAIN_COLLISION),
        command("NavMesh", TOGGLE_NAVMESH), command("AI Sensors", TOGGLE_AI_SENSORS),
        command("Decal Bounds", TOGGLE_DECAL_BOUNDS),
        command("Terrain Bounds", TOGGLE_TERRAIN_BOUNDS), command("LOD Bounds", TOGGLE_LOD_BOUNDS),
        command("Hot Reload", TOGGLE_HOTRELOAD), command("Hot Reload Sound", TOGGLE_HOTRELOAD_SOUND),
        separator(),
        command("Shadow", TOGGLE_SHADOW),
        submenu("View Mode", std::move(debugViewMode))
    }));
    menus.push_back(submenu("Tools", {
        command("Map Editing Mode", TOGGLE_MAP),
        submenu("Terrain & Map", std::move(terrainTools)),
        command("Build Settings...", OPEN_BUILD), command("IBL Baker...", OPEN_IBL),
        command("Volume Flipbook Baker...", OPEN_VOLUME_FLIPBOOK),
        command("Navigation...", OPEN_NAVIGATION),
        command("Asset Maintenance...", OPEN_ASSET_MAINT)
    }));
    menus.push_back(submenu("AI", { command("AI Settings...", OPEN_AI_SETTINGS) }));

    m_window->SetNativeMenu(std::move(menus), [this](uint16_t id) {
        if (id >= PANEL_BASE && id < PANEL_BASE + m_panels.size()) {
            OpArgs args;
            args.Set("panel", std::string(m_panels[id - PANEL_BASE]->GetWindowName()));
            InvokeOperator("panel.set_visible", args);
            return true;
        }
        if (id >= PRESET_BASE && id < PRESET_BASE + ObjectPresetCatalog().size()) {
            OpArgs args;
            args.Set("preset", std::string(ObjectPresetCatalog()[static_cast<std::size_t>(id - PRESET_BASE)].id));
            InvokeOperator("node.create_preset", args);
            return true;
        }

        /// @note ネイティブメニューは項目ごとの有効/無効を持たないが、InvokeOperator は poll を
        ///       満たさない要求を拒否するので、グレーアウトできない面でも同じ条件が効く。
        switch (id) {
        case NEW_SCENE:       InvokeOperator("scene.new"); break;
        case OPEN_SCENE:      InvokeOperator("scene.open"); break;
        case SAVE_SCENE:      InvokeOperator("scene.save"); break;
        case SAVE_SCENE_AS:   InvokeOperator("scene.save_as"); break;
        case CLOSE_PREFAB:    InvokeOperator("prefab.close"); break;
        case SAVE_ALL_ASSETS: InvokeOperator("asset.save_all"); break;
        case EXIT_EDITOR:     InvokeOperator("app.exit"); break;
        case UNDO:            InvokeOperator("edit.undo"); break;
        case REDO:            InvokeOperator("edit.redo"); break;
        case CREATE_EMPTY:    InvokeOperator("node.create_empty"); break;
        case PLAY:            InvokeOperator("play.start"); break;
        case STOP:            InvokeOperator("play.stop"); break;
        case PAUSE:           InvokeOperator("play.pause"); break;
        case STEP:            InvokeOperator("play.step"); break;
        case RELOAD_SCRIPTS:  InvokeOperator("script.reload"); break;
        case OPEN_ANALYSIS:   InvokeOperator("tools.analysis"); break;
        case TOGGLE_PASS_VIEWER: {
            OpArgs args;
            args.Set("panel", std::string("Render Pass Viewer"));
            InvokeOperator("panel.set_visible", args);
            break;
        }
        /// @note 表示トグルとビューモードは Operator を通す。同じ切り替えを 3 面が持つので、
        ///       書き込み先 (EditorContext か ProjectSettings か) を各面が覚えていると必ずずれる。
        case TOGGLE_GRID:      InvokeOperator("render.show_grid"); break;
        case TOGGLE_LIGHTS:    InvokeOperator("render.show_light_range"); break;
        case TOGGLE_VFX:       InvokeOperator("render.show_vfx_gizmos"); break;
        case TOGGLE_SKELETON:  InvokeOperator("render.show_skeleton"); break;
        case TOGGLE_STATS:     InvokeOperator("render.show_stats"); break;
        case TOGGLE_HOTRELOAD: InvokeOperator("debug.hot_reload"); break;
        case TOGGLE_HOTRELOAD_SOUND: InvokeOperator("debug.hot_reload_sound"); break;
        case TOGGLE_COLLIDERS: InvokeOperator("render.show_colliders"); break;
        case TOGGLE_TERRAIN_COLLISION: InvokeOperator("render.show_terrain_collision"); break;
        case TOGGLE_NAVMESH:    InvokeOperator("render.show_navmesh"); break;
        case TOGGLE_AI_SENSORS: InvokeOperator("render.show_nav_sensors"); break;
        case TOGGLE_DECAL_BOUNDS: InvokeOperator("render.show_decal_bounds"); break;
        case TOGGLE_SCENE_ICONS:    InvokeOperator("render.show_scene_icons"); break;
        case TOGGLE_SCRIPT_GIZMOS:  InvokeOperator("render.show_script_gizmos"); break;
        case TOGGLE_SKELETON_SEL:   InvokeOperator("render.skeleton_selected_only"); break;
        case TOGGLE_CONSTRAINTS:    InvokeOperator("render.show_constraints"); break;
        case TOGGLE_RIGID_BODIES:   InvokeOperator("render.show_rigid_bodies"); break;
        case TOGGLE_IK:             InvokeOperator("render.show_ik"); break;
        case TOGGLE_SPRING_BONES:   InvokeOperator("render.show_spring_bones"); break;
        case TOGGLE_ATTACHMENTS:    InvokeOperator("render.show_attachments"); break;
        case TOGGLE_VFX_PATHS:      InvokeOperator("render.show_vfx_paths"); break;
        case TOGGLE_TERRAIN_BOUNDS: InvokeOperator("render.show_terrain_bounds"); break;
        case TOGGLE_LOD_BOUNDS:     InvokeOperator("render.show_lod_bounds"); break;
        case TOGGLE_FLOW_FIELDS:    InvokeOperator("render.show_flow_fields"); break;
        case TOGGLE_FLOW_SAMPLES:   InvokeOperator("render.show_flow_samples"); break;
        case TOGGLE_PHYSICS_VOLUMES: InvokeOperator("render.show_physics_volumes"); break;
        case TOGGLE_WATER_FLOW:     InvokeOperator("render.show_water_flow"); break;
        case TOGGLE_SHADOW:  InvokeOperator("render.shadow_enabled"); break;
        case VIEW_LIT:       InvokeViewMode("lit"); break;
        case VIEW_UNLIT:     InvokeViewMode("unlit"); break;
        case VIEW_WIRE_LIT:  InvokeViewMode("wireframe_lit"); break;
        case VIEW_WIRE_UNLIT:InvokeViewMode("wireframe_unlit"); break;
        case 700:            InvokeOperator("view.reset_ui_scale"); break;
        case TOGGLE_MAP:     InvokeOperator("tools.map_editing_mode"); break;
        case OPEN_BUILD:     InvokeOperator("tools.build_settings"); break;
        /// @note パネルを前面に出すのは panel.focus 1 つで足りる。パネルごとに
        ///       operator を生やすと m_panels という単一の出所が二重管理へ戻る。
        case OPEN_IBL:         InvokePanelFocus(m_iblBakePanel); break;
        case OPEN_VOLUME_FLIPBOOK: InvokePanelFocus(m_volumeFlipbookBakePanel); break;
        case OPEN_NAVIGATION:  InvokePanelFocus(m_navigationPanel); break;
        case OPEN_ASSET_MAINT: InvokePanelFocus(m_assetMaintenancePanel); break;
        case OPEN_AI_SETTINGS: InvokePanelFocus(m_aiSettingsPanel); break;
        case 510: InvokeOperator("tools.terrain"); break;
        default: return false;
        }
        return true;
    });
}

void EditorApp::BuildMenuBar(EditorContext& ctx)
{
    if (!ImGui::BeginMenuBar()) return;

    /// @note FBZZ Studio のブランドマーク。OS タイトルバーを隠す最大化・マルチビューポート環境でも製品識別を保つ。
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Accent));
    ImGui::TextUnformatted("FBZZ");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::TextMuted));
    ImGui::TextUnformatted("STUDIO");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    /// @name File
    if (ImGui::BeginMenu(LOC("File"))) {
        /// @note Prefab 編集中は項目名を "Save Prefab" にし、保存対象が Prefab アセットであることを明示する。
        const bool inPrefabEdit = ctx.InPrefabEditMode();
        MenuItemOp("scene.new");
        MenuItemOp("scene.open");
        MenuItemOp("scene.save", inPrefabEdit ? "Save Prefab" : nullptr);
        MenuItemOp("scene.save_as");
        /// @note Prefab 編集中だけ出す。poll (prefab.close) も同じ条件を持つので、
        ///       表示していない状況では AI からも通らない。
        if (inPrefabEdit) MenuItemOp("prefab.close");
        MenuItemOp("asset.save_all");
        ImGui::Separator();
        MenuItemOp("app.exit");
        ImGui::EndMenu();
    }

    /// @name Edit
    if (ImGui::BeginMenu(LOC("Edit"))) {
        /// @note 直前の操作名を添えて「何が戻るのか」を読めるようにする。
        ///       ラベルだけが動的で、実行可否と実体は operator 側にある。
        const bool canUndo = ctx.undoStack && ctx.undoStack->CanUndo();
        const bool canRedo = ctx.undoStack && ctx.undoStack->CanRedo();
        const std::string undoLabel = canUndo
            ? "Undo " + ctx.undoStack->GetUndoDescription()
            : "Undo";
        const std::string redoLabel = canRedo
            ? "Redo " + ctx.undoStack->GetRedoDescription()
            : "Redo";
        MenuItemOp("edit.undo", undoLabel.c_str());
        MenuItemOp("edit.redo", redoLabel.c_str());
        ImGui::Separator();
        MenuItemOp("edit.duplicate");
        MenuItemOp("edit.delete_selected");
        ImGui::Separator();
        MenuItemOp("edit.copy");
        MenuItemOp("edit.paste");
        MenuItemOp("edit.paste_as_child");
        ImGui::EndMenu();
    }

    /// @note 中身は Hierarchy の右クリックと同じ投影 (CreateObjectMenu)。選択を既定の親にする。
    if (ImGui::BeginMenu(LOC("GameObject"))) {
        PendingObjectCreate pendingCreate;
        MenuItemOp("node.create_empty");
        ImGui::Separator();
        DrawCreateObjectMenu(ctx, pendingCreate);
        ImGui::EndMenu();
        InvokePendingObjectCreate(ctx, pendingCreate);
    }

    /// @name View
    if (ImGui::BeginMenu(LOC("View"))) {
        /// @note panel.set_visible の投影にすることで、人が押すのと同じ実体を AI も呼べる
        ///       (パネルが増えても operator は 1 つのまま)。
        if (ImGui::BeginMenu(LOC("Panels"))) {
            for (auto& panel : m_panels) {
                if (!panel->ShowInViewMenu()) continue;
                OpArgs args;
                args.Set("panel", std::string(panel->GetWindowName()));
                MenuItemOpArgs("panel.set_visible", args, panel->GetViewMenuName());
            }
            ImGui::EndMenu();
        }

        /// @note Scene View の視点。軸ビューはナビゲーションギズモのクリックと同じ操作を指す。
        if (ImGui::BeginMenu(LOC("Scene Camera"))) {
            MenuItemOp("view.toggle_projection");
            ImGui::Separator();
            MenuItemOp("view.axis_front");
            MenuItemOp("view.axis_back");
            MenuItemOp("view.axis_left");
            MenuItemOp("view.axis_right");
            MenuItemOp("view.axis_top");
            MenuItemOp("view.axis_bottom");
            ImGui::Separator();
            MenuItemOp("view.frame_selected");
            ImGui::EndMenu();
        }

        /// @note UI 全体スケール (フォント + 余白)。設定に永続化される。
        ///       スライダー自体は連続値のドラッグなので operator には乗らないが、
        ///       適用は view.set_ui_scale を通す (範囲の宣言と適用処理を 1 箇所に保つ)。
        ImGui::Separator();
        ImGui::TextDisabled("%s", LOCT("UI Scale"));
        /// @note 範囲は operator の params 宣言から引く。スライダー側に直書きすると、
        ///       人は 0.5x にできるのに AI からは BAD_ARG で弾かれる (同じ操作の限界が
        ///       面ごとに違う) という、この設計が消したいずれが範囲という形で再発する。
        float scaleMin = 0.7f;
        float scaleMax = 2.0f;
        if (const EditorOperator* scaleOp = m_operators.Find("view.set_ui_scale");
            scaleOp != nullptr && !scaleOp->params.empty() && scaleOp->params[0].hasRange) {
            scaleMin = scaleOp->params[0].minValue;
            scaleMax = scaleOp->params[0].maxValue;
        }
        float uiScale = m_ctx.editorUiScale;
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::SliderFloat("##ui_scale", &uiScale, scaleMin, scaleMax, "%.2fx")) {
            OpArgs args;
            args.Set("scale", uiScale);
            InvokeOperator("view.set_ui_scale", args);
        }
        MenuItemOp("view.reset_ui_scale");
        ImGui::EndMenu();
    }

    /// @name Debug
    if (ImGui::BeginMenu(LOC("Debug"))) {
        /// @note View はレイアウト・パネル、Debug は実行/描画診断に役割を分ける。
        MenuItemOp("tools.analysis", "Analysis");
        {
            OpArgs args;
            args.Set("panel", std::string("Render Pass Viewer"));
            MenuItemOpArgs("panel.set_visible", args, "Render Pass Viewer");
        }
        ImGui::Separator();
        /// @note 全項目を operator の投影にする。フラグのアドレスを直接渡すと、同じフラグを
        ///       切り替える operator と表示が別経路になり、AI からも見えない。
        /// @name Scene Overlays
        MenuItemOp("render.show_grid",        "Grid");
        MenuItemOp("render.show_light_range", "Light Range");
        MenuItemOp("render.show_vfx_gizmos",  "VFX Emitters");
        MenuItemOp("render.show_flow_fields", "Flow Fields");
        MenuItemOp("render.show_flow_samples", "Flow Samples");
        MenuItemOp("render.show_skeleton",    "Skeleton");
        MenuItemOp("render.skeleton_selected_only", "Skeleton: Selected Only");
        MenuItemOp("render.show_stats",       "Stats");
        MenuItemOp("render.show_scene_icons", "Scene Icons");
        MenuItemOp("render.show_script_gizmos", "Script Gizmos");
        MenuItemOp("render.show_ik",            "IK Chains");
        MenuItemOp("render.show_spring_bones",  "Spring Bones");
        MenuItemOp("render.show_attachments",   "Attachments");
        MenuItemOp("render.show_vfx_paths",     "VFX Paths");
        ImGui::Separator();
        /// @name Physics / Rendering
        MenuItemOp("render.show_colliders",         "Colliders");
        MenuItemOp("render.show_constraints",       "Constraints");
        MenuItemOp("render.show_rigid_bodies",      "Rigid Bodies");
        MenuItemOp("render.show_physics_volumes",   "Physics Volumes");
        MenuItemOp("render.show_water_flow",        "Water Flow");
        MenuItemOp("render.show_terrain_collision", "Terrain Collision");
        MenuItemOp("render.show_navmesh",           "NavMesh");
        if (ImGui::BeginMenu(LOC("NavMesh Draw Mode"))) {
            const auto navModeItem = [this](const char* mode, const char* label) {
                OpArgs args;
                args.Set("mode", std::string(mode));
                MenuItemOpArgs("render.set_navmesh_draw_mode", args, label);
            };
            navModeItem("solid",       "Solid");
            navModeItem("transparent", "Transparent");
            navModeItem("areas",       "Areas");
            navModeItem("portals",     "Portals");
            navModeItem("voxels",      "Voxels");
            ImGui::EndMenu();
        }
        MenuItemOp("render.show_nav_sensors",       "AI Sensors");
        MenuItemOp("render.show_decal_bounds",      "Decal Bounds");
        MenuItemOp("render.show_terrain_bounds",    "Terrain Bounds");
        MenuItemOp("render.show_lod_bounds",        "LOD Bounds");
        ImGui::Separator();
        /// @name Culling
        /// @note Scene View はデバッグカメラで描くため CameraComponent の設定が届かない。
        ///       「消えた原因がカリングか」を切り分ける唯一の口なので Debug 側へ出す。
        MenuItemOp("render.scene_view_occlusion_culling", "Scene View Occlusion Culling");
        ImGui::Separator();
        /// @name Tools
        MenuItemOp("debug.hot_reload", "Hot Reload");
        MenuItemOp("debug.hot_reload_sound", "Hot Reload Sound");
        ImGui::Separator();
        if (ImGui::BeginMenu(LOC("View Mode"))) {
            /// @note 排他選択は 1 つの operator に引数で渡す。チェックは checked を
            ///       同じ引数で評価した値なので、「表示は Lit なのに実体は Unlit」が作れない。
            const auto viewModeItem = [this](const char* mode, const char* label) {
                OpArgs args;
                args.Set("mode", std::string(mode));
                MenuItemOpArgs("render.set_view_mode", args, label);
            };
            viewModeItem("lit",             "Lit");
            viewModeItem("unlit",           "Unlit");
            viewModeItem("wireframe_lit",   "Wireframe Lit");
            viewModeItem("wireframe_unlit", "Wireframe Unlit");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        /// @note Bloom / FXAA は Post Process Volume + Profile (.fzdata) へ一本化済み。
        ///       Shadow はプロジェクト全体の描画構成なのでここに残す。
        MenuItemOp("render.shadow_enabled", "Shadow");
        ImGui::Separator();
        /// @name Developer
        /// @see Docs/design/developer-mode.md
        MenuItemOp("developer.toggle_mode", "Developer Mode");
        if (core::DeveloperMode::IsEnabled() && ImGui::BeginMenu(LOC("Developer"))) {
            if (ImGui::BeginMenu(LOC("Crash"))) {
                const auto crashItem = [this](const char* kind, const char* label) {
                    OpArgs args;
                    args.Set("kind", std::string(kind));
                    MenuItemOpArgs("developer.crash", args, label);
                };
                crashItem("report",            "Write Report Only");
                ImGui::Separator();
                crashItem("access_violation",  "Access Violation");
                crashItem("stack_overflow",    "Stack Overflow");
                crashItem("abort",             "abort()");
                crashItem("pure_call",         "Pure Virtual Call");
                crashItem("invalid_parameter", "CRT Invalid Parameter");
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(LOC("Tools"))) {
        MenuItemOp("tools.map_editing_mode");
        ImGui::Separator();
        if (ImGui::BeginMenu(LOC("Terrain & Map"))) {
            MenuItemOp("tools.terrain");
            ImGui::EndMenu();
        }
        MenuItemOp("tools.build_settings");
        ImGui::Separator();
        /// @note パネルごとに operator を生やすと m_panels という単一の出所が二重管理へ戻る。
        ///       名前を引数で渡す 1 つの操作で足りる。
        if (m_iblBakePanel && ImGui::MenuItem(LOC("IBL Baker...")))
            InvokePanelFocus(m_iblBakePanel);
        if (m_volumeFlipbookBakePanel && ImGui::MenuItem(LOC("Volume Flipbook Baker...")))
            InvokePanelFocus(m_volumeFlipbookBakePanel);
        if (m_navigationPanel && ImGui::MenuItem(LOC("Navigation...")))
            InvokePanelFocus(m_navigationPanel);
        if (m_assetMaintenancePanel && ImGui::MenuItem(LOC("Asset Maintenance...")))
            InvokePanelFocus(m_assetMaintenancePanel);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(LOC("AI"))) {
        if (m_aiSettingsPanel && ImGui::MenuItem(LOC("AI Settings...")))
            InvokePanelFocus(m_aiSettingsPanel);
        ImGui::Separator();
        /// @note チェックマークは operator の checked (= 実際の待受状態) なので、
        ///       状態表示の役割を保ったままその場で切り替えられる。
        MenuItemOp("ai.command_bus", "Editor Command Bus");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(LOC("Help"))) {
        if (ImGui::MenuItem(LOC("About FBZZ Studio..."))) m_aboutRequested = true;
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

/// @name About

/// @brief 起動時に「何を触っているか」を名乗るダイアログ。名前・構成・出所を 1 画面にまとめる。
void EditorApp::DrawAboutDialog()
{
    constexpr const char* kAboutPopupId = "##fbzz_about";
    if (m_aboutRequested) {
        ImGui::OpenPopup(kAboutPopupId);
        m_aboutRequested = false;
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, { 0.5f, 0.5f });
    ImGui::SetNextWindowSize({ ImGui::GetFontSize() * 24.0f, 0.0f }, ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal(kAboutPopupId, nullptr,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings))
        return;

    widgets::BeginHeadingFont(2.2f);
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::Accent));
    ImGui::TextUnformatted("FBZZ");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.28f);
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(ThemeColor::TextMuted));
    ImGui::TextUnformatted("STUDIO");
    ImGui::PopStyleColor();
    widgets::EndHeadingFont();

    ImGui::Spacing();
    ImGui::TextDisabled("%s", LOCT("A C++20 game engine and editor built from scratch."));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    /// @note 何で動いているかを 1 行ずつ。«自作» の範囲がここで読み取れるようにする。
    const auto row = [](const char* label, const char* value) {
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
        ImGui::TextUnformatted(value);
    };
    row(LOCT("Renderer"), m_ctx.renderer != nullptr ? m_ctx.renderer->GetBackendName() : "-");
#if defined(_DEBUG)
    row(LOCT("Build"), "Debug");
#else
    row(LOCT("Build"), "Release");
#endif
    row(LOCT("Built"), __DATE__);
    row(LOCT("Author"), "Hasegawa Jin");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button(LOC("Close"), { -1.0f, 0.0f })) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void EditorApp::BuildPlayToolbar(EditorContext& ctx)
{
    /// @note Play 系操作を上部中央へ独立配置し、メニュー項目より実行状態を読み取りやすくする。
    constexpr float TOOLBAR_HEIGHT = 34.0f;
    const ImVec2 BUTTON_SIZE { 34.0f, 24.0f };
    constexpr float BUTTON_SPACING = 4.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 8.0f, 5.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { BUTTON_SPACING, 0.0f });
    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(ThemeColor::Surface));
    ImGui::BeginChild("##MainPlayToolbar", { 0.0f, TOOLBAR_HEIGHT }, false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    PlayModeController* pm = ctx.playMode;
    const bool isEditor = pm && pm->IsInEditor();
    const bool isPlaying = pm && pm->IsPlaying();
    const bool isPaused = pm && pm->IsPaused();

    const float groupWidth = BUTTON_SIZE.x * 5.0f + BUTTON_SPACING * 4.0f;
    const float availableWidth = ImGui::GetWindowWidth();
    const float centerOffset = (std::max)(8.0f, (availableWidth - groupWidth) * 0.5f);
    ImGui::SetCursorPosX(centerOffset);
    const bool scriptReloadBusy =
        ctx.scriptReloadBusy ||
        ctx.hotReloadState == EditorContext::HotReloadState::Compiling ||
        ctx.hotReloadState == EditorContext::HotReloadState::Reloading;
    const char* playTooltip = scriptReloadBusy
        ? "Play is available after scripts finish compiling/reloading"
        : (isPaused ? "Resume from Play Mode" : "Play");

    const ImVec4 playColor  = EditorTheme::Color(ThemeColor::Success);
    const ImVec4 stopColor  = EditorTheme::Color(ThemeColor::Danger);
    const ImVec4 pauseColor = EditorTheme::Color(ThemeColor::Warning);

    if (PlayToolbarButton(
        "##PlayModePlay",
        playTooltip,
        PlayToolbarIcon::Play,
        CanInvokeOperator("play.start"),
        isPlaying,
        playColor,
        BUTTON_SIZE)) {
        InvokeOperator("play.start");
    }

    ImGui::SameLine();
    if (PlayToolbarButton(
        "##PlayModeStop",
        "Stop",
        PlayToolbarIcon::Stop,
        CanInvokeOperator("play.stop"),
        isEditor,
        stopColor,
        BUTTON_SIZE)) {
        InvokeOperator("play.stop");
    }

    ImGui::SameLine();
    if (PlayToolbarButton(
        "##PlayModePause",
        isPaused ? "Resume" : "Pause",
        PlayToolbarIcon::Pause,
        CanInvokeOperator("play.pause"),
        isPaused,
        pauseColor,
        BUTTON_SIZE))
        InvokeOperator("play.pause");

    ImGui::SameLine();
    if (PlayToolbarButton(
        "##PlayModeStep",
        "Step",
        PlayToolbarIcon::Step,
        CanInvokeOperator("play.step"),
        false,
        pauseColor,
        BUTTON_SIZE))
        InvokeOperator("play.step");

    ImGui::SameLine();
    {
        const ImVec4 reloadColor { 0.25f, 0.55f, 0.90f, 1.0f };
        /// @note Blueprint の Compile ボタンと同じく、アイコン自体が直近の結果を持ち続ける (時間で消えない)。
        using Result = EditorContext::ScriptReloadResult;
        PlayToolbarIcon reloadIcon    = PlayToolbarIcon::Reload;
        const char*     reloadTooltip = "Reload Scripts";
        ImVec4          iconColor     = EditorTheme::Color(ThemeColor::Text);
        const bool      tintIcon      = scriptReloadBusy || ctx.scriptReloadResult != Result::None;
        if (scriptReloadBusy) {
            reloadIcon    = PlayToolbarIcon::ReloadBusy;
            reloadTooltip = "Scripts are compiling / reloading...";
            iconColor     = EditorTheme::Color(ThemeColor::Warning);
        } else if (ctx.scriptReloadResult == Result::Failed) {
            reloadIcon    = PlayToolbarIcon::ReloadFailed;
            reloadTooltip = "Last script reload failed \xE2\x80\x94 click to rebuild";
            iconColor     = EditorTheme::Color(ThemeColor::Danger);
        } else if (ctx.scriptReloadResult == Result::Ok) {
            reloadIcon    = PlayToolbarIcon::ReloadOk;
            reloadTooltip = "Scripts are up to date \xE2\x80\x94 click to rebuild";
            iconColor     = EditorTheme::Color(ThemeColor::Success);
        }
        if (PlayToolbarButton(
            "##ScriptReload",
            reloadTooltip,
            reloadIcon,
            CanInvokeOperator("script.reload"),
            scriptReloadBusy,
            reloadColor,
            BUTTON_SIZE,
            tintIcon ? &iconColor : nullptr))
            InvokeOperator("script.reload");
    }

    /// @name 右端: ホットリロードステータス / PLAYING ラベル
    {
        /// @note ホットリロードステータステキストを決定する
        const bool  shaders     = ctx.hotReloadTarget == EditorContext::HotReloadTarget::Shaders;
        const char* reloadText  = nullptr;
        ImVec4      reloadColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        switch (ctx.hotReloadState) {
        case EditorContext::HotReloadState::Compiling:
            reloadText  = shaders ? "Compiling shaders..." : "Compiling scripts...";
            reloadColor = { 1.0f, 0.85f, 0.2f,  1.0f };
            break;
        case EditorContext::HotReloadState::Reloading:
            reloadText  = "Reloading scripts...";
            reloadColor = { 0.5f, 0.8f,  1.0f,  1.0f };
            break;
        case EditorContext::HotReloadState::Done:
            reloadText  = shaders ? "Shaders reloaded" : "Scripts reloaded";
            reloadColor = { 0.35f, 1.0f, 0.45f, 1.0f };
            break;
        case EditorContext::HotReloadState::Failed:
            /// @note コンパイル失敗とロード失敗を区別する。以前はどちらも «Compile Error» だった。
            reloadText  = ctx.hotReloadMessage.find("error") != std::string::npos
                ? (shaders ? "Shader errors" : "Script errors")
                : (shaders ? "Shader reload failed" : "Script reload failed");
            reloadColor = { 1.0f, 0.35f, 0.35f, 1.0f };
            break;
        default: break;
        }

        /// @note PLAYING / PAUSED ラベル
        const char*  playLabel    = !isEditor ? (isPlaying ? "PLAYING" : "PAUSED") : nullptr;
        const ImVec4 playLabelCol = isPlaying
            ? ImVec4{ 0.28f, 0.88f, 0.53f, 1.0f }
            : ImVec4{ 0.92f, 0.72f, 0.28f, 1.0f };

        /// @note 右端からテキスト幅で逆算して配置する (描画対象がある場合のみ)
        constexpr float kGap = 6.0f;
        float totalW = 0.0f;
        if (reloadText) totalW += ImGui::CalcTextSize(reloadText).x + kGap;
        if (playLabel)  totalW += ImGui::CalcTextSize(playLabel).x  + kGap;

        if (totalW > 0.0f) {
            const float groupRight = centerOffset + groupWidth;
            const float posX = availableWidth - totalW - 8.0f;
            const float posY = (TOOLBAR_HEIGHT - ImGui::GetTextLineHeight()) * 0.5f;
            /// @note ステータス文字をボタン群へ重ねず、狭いフレームでは表示を省略する。
            ///       Compiling / PLAYING 表示が Play ボタンを圧迫するより、操作ボタンの完全表示を優先する。
            if (posX > groupRight + kGap) {
                if (posX > ImGui::GetCursorPosX())
                    ImGui::SetCursorPos({ posX, posY });

                if (reloadText) {
                    ImGui::PushStyleColor(ImGuiCol_Text, reloadColor);
                    ImGui::TextUnformatted(reloadText);
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered() && !ctx.hotReloadMessage.empty())
                        ImGui::SetTooltip("%s\nClick to open Build Output", ctx.hotReloadMessage.c_str());
                    if (ImGui::IsItemClicked()) ctx.requestOpenBuildOutput = true;
                    if (playLabel) ImGui::SameLine(0.0f, kGap);
                }
                if (playLabel)
                    ImGui::TextColored(playLabelCol, "%s", playLabel);
            }
        }
    }

    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 max = { min.x + ImGui::GetWindowWidth(), min.y + ImGui::GetWindowHeight() };
    ImGui::GetWindowDrawList()->AddLine({ min.x, max.y - 1.0f }, { max.x, max.y - 1.0f },
        ImGui::GetColorU32(ImGuiCol_Separator));

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

/// ビルド失敗通知バー

/// @note ビルド失敗が残っている間はツールバー直下に赤帯を出し続け、[Show] / [Dismiss] で操作する。
///       成功ビルドで自動的に消える。
void EditorApp::DrawBuildNotificationBar(EditorContext& ctx)
{
    if (!ctx.buildConsole || !ctx.buildConsole->HasActiveFailure()) return;

    const BuildRecord* fail = ctx.buildConsole->LatestFailure();
    if (!fail) return;

    const float barH = ImGui::GetFrameHeight() + 4.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(ThemeColor::SurfaceRaised));
    ImGui::BeginChild("##BuildFailBar", ImVec2(0.0f, barH), false, ImGuiWindowFlags_NoScrollbar);

    ImGui::AlignTextToFramePadding();
    const char* kind = fail->kind == BuildRecord::Kind::Script ? "Script" : "HLSL";
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.50f, 1.0f),
                       "  %s build failed  \xE2\x80\x94  %d error(s), %d warning(s)  [%s]",
                       kind, fail->errorCount, fail->warnCount, fail->startClock.c_str());

    /// @note 右寄せで操作ボタンを置く。
    const float btnW = 150.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - btnW);
    if (ImGui::SmallButton("Show")) {
        /// @note Build Output を開いて最初のエラーへスクロール
        ctx.requestFocusBuildError = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Dismiss")) {
        ctx.buildConsole->DismissNotification();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void EditorApp::DrawGuidConflictBar(EditorContext& /*ctx*/)
{
    const size_t count = asset::AssetDatabase::GuidConflictCount();
    if (count == 0) return;
    /// @note 閉じた後に «増えた» ときだけ出し直す。同じ件数のまま出し続けない。
    if (m_guidConflictBarDismissed && count <= m_dismissedGuidConflicts) return;
    m_guidConflictBarDismissed = false;

    const float barH = ImGui::GetFrameHeight() + 4.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(ThemeColor::SurfaceRaised));
    ImGui::BeginChild("##GuidConflictBar", ImVec2(0.0f, barH), false, ImGuiWindowFlags_NoScrollbar);

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
                       "  %zu duplicate guid(s)  \xE2\x80\x94  "
                       "references resolve to one side only", count);

    const float btnW = 150.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - btnW);
    if (ImGui::SmallButton("Fix...") && m_assetMaintenancePanel)
        InvokePanelFocus(m_assetMaintenancePanel);
    ImGui::SameLine();
    if (ImGui::SmallButton("Dismiss##guid")) {
        m_guidConflictBarDismissed = true;
        m_dismissedGuidConflicts   = count;
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

/// ホットキー登録

void EditorApp::StartPlayMode()
{
    /// @note Play ツールバーボタンと Ctrl+P ホットキーの共通経路。
    ///       ガード条件をここに集約し、どの入力経路でも同じ前提チェックを通す。
    const bool scriptReloadBusy =
        m_ctx.scriptReloadBusy ||
        m_ctx.hotReloadState == EditorContext::HotReloadState::Compiling ||
        m_ctx.hotReloadState == EditorContext::HotReloadState::Reloading;
    if (!m_ctx.activeScene || !m_playMode.IsInEditor() || scriptReloadBusy)
        return;
    if (!m_scriptDll.IsLoaded() && !m_scriptDllPath.empty()) {
        m_ctx.requestScriptReload = true;
        Toast::Error("Scripts unavailable. Rebuilding; press Play again after success.");
        FBZZ_LOG_WARN("Play: Scripts DLL is unavailable; rebuild requested");
        return;
    }
    /// @note Prefab 編集面には「シーン」が無い (カメラもライトも無い、プレファブ単体)。
    ///       そのまま Play すると空舞台で動き出し、Stop 時の復元も Prefab 内容に対して行われるため、
    ///       編集面から出るまで開始させない。
    if (m_ctx.InPrefabEditMode()) {
        FBZZ_LOG_WARN("Play: close the prefab edit mode first");
        return;
    }

    /// @note FBZZ_REQUIRE_COMPONENT の充足をシーン全体でまとめて検証する。
    ///       付け忘れは「動かないけどエラーも出ない」形でしか現れないので、Play を押した瞬間に
    ///       Console へ全件出す。ただし Play は止めない — 作りかけを走らせるのが Play の役目。
    if (const auto issues = scene::ValidateSceneScriptRequirements(*m_ctx.activeScene);
        !issues.empty()) {
        for (const auto& issue : issues)
            FBZZ_LOG_ERROR("Script requirement: %s",
                           scene::FormatScriptRequirementIssue(issue).c_str());
        Toast::Error(std::to_string(issues.size()) +
                     " missing script component(s) — see Console");
    }

    m_undoStack.Clear();
    /// @note 編集中に何かが基底を書いていても、Play は «誰も要求していない» 状態から始める。
    ///       UpdatePlayCursorControls (フレーム先頭) より前に畳む必要がある — 次フレームには
    ///       既に OnStart が要求を積んでおり、あちらで畳むと名乗ったばかりの要求を消してしまう。
    core::Cursor::ClearRequests();
    /// @note カーソルの絵もここで読む。スクリプトの OnStart は «OS カーソルの絵があるか» で
    ///       自前のポインターを出すかどうかを決めるので、走り出す前に揃っている必要がある。
    m_ctx.projectSettings.cursor.Apply(m_ctx.projectRoot);
    /// @note Play 前に editor-only 非表示を一時解除（スナップショットに active 状態で含める）
    RemoveEditorHiding();
    /// @note navMesh は TOML に保存されないため、Play 開始前にキャッシュしておく。
    ///       Stop 後の scene 復元で needsBake=true が立っても再ベイクせずに済む。
    m_navMeshPlayCache.clear();
    for (scene::EntityID eid : m_ctx.activeScene->GetEntities<scene::NavMeshSurfaceComponent>()) {
        auto* surf = m_ctx.activeScene->GetComponent<scene::NavMeshSurfaceComponent>(eid);
        auto* go   = m_ctx.activeScene->GetGameObject(eid);
        if (!surf || !go || !surf->navMesh.IsValid()) continue;
        NavMeshPlayCacheEntry entry;
        entry.navMesh    = surf->navMesh;
        entry.stats      = surf->bakeStats;
        entry.sourceHash = surf->bakedSourceHash;
        /// @note ボクセル格子は Scene View の Voxels 表示専用で、Play 中は表示自体が抑止される。
        ///       数 MB を二重に抱えないよう、預けている間は Surface から外す。
        entry.debug      = std::move(surf->bakeDebug);
        surf->bakeDebug  = {};
        m_navMeshPlayCache[go->instanceId] = std::move(entry);
    }
    /// @note ScriptProxy は ScriptRuntime 経由でサブシステムを参照する。エディタは共通
    ///       ProjectRuntime の SceneManager を Update するため、Play 開始時に ScriptRuntime を
    ///       差し替えて正しい参照先を指す。
    m_runtime.ActivateScriptRuntime(
        core::Application::Get().GetRenderer(),
        static_cast<uint32_t>(m_ctx.gameViewportWidth),
        static_cast<uint32_t>(m_ctx.gameViewportHeight)
    );
    /// @note graphics プロキシの書き換え先を Play 中だけ開ける。実体は ProjectSettings::render で
    ///       終了時に toml へ保存されるので、スナップショットを取らないと Play 中の変更が焼き付く。
    m_renderSettingsPlaySnapshot = m_ctx.projectSettings.render;
    core::Application::Get().SetActiveRenderSettings(&m_ctx.projectSettings.render);
    /// @note Play 中の増加も Stop 後の残りも、この 1 つの基準から測る。
    if (m_ctx.resources != nullptr) {
        m_memoryLeakDiff.CaptureBaseline(*m_ctx.resources, "Play");
        /// @note 最初の Play だけ «セッション基準» も置く。キャッシュの初回充填と、
        ///       往復のたびに積むリークを、1 往復ぶんの差分だけでは区別できないため。
        m_memoryLeakDiff.CaptureSessionBaselineIfAbsent(*m_ctx.resources);
    }
    m_playMode.Play(*m_ctx.activeScene);
    if (m_playMode.IsPlaying() && m_ctx.playFocusMode != EditorContext::PlayFocusMode::Unfocused)
        m_ctx.requestGameViewportFocus = true;
}

void EditorApp::StopPlayMode()
{
    if (!m_ctx.activeScene || m_playMode.IsInEditor())
        return;
    scene::ScriptRuntime::Override(nullptr);
    /// @note Play 中のスクリプトが変えた画質・明るさを編集側へ持ち込まない。
    ///       選択状態だけは編集の続きなので、復元から外して現在のものを残す。
    core::Application::Get().SetActiveRenderSettings(nullptr);
    {
        auto selection = std::move(m_ctx.projectSettings.render.selectedObjects);
        m_ctx.projectSettings.render = m_renderSettingsPlaySnapshot;
        m_ctx.projectSettings.render.selectedObjects = std::move(selection);
    }
    /// @note AudioSystemはSimOnlyのため、EditModeへ戻った後ではループVoiceを停止できない。
    ///       PauseではなくPlay終了時だけ一括停止し、BGMがEditor操作中まで残ることを防ぐ。
    if (auto* audioManager = core::Application::Get().GetAudioManager())
        audioManager->StopAllVoices();
    m_playMode.Stop(*m_ctx.activeScene);
    m_undoStack.Clear();
}

void EditorApp::TogglePlayMode()
{
    if (m_playMode.IsInEditor())
        StartPlayMode();
    else
        StopPlayMode();
}

/// 描画モードを operator へ渡す小さな補助 (ネイティブメニューの 4 項目が使う)。
void EditorApp::InvokeViewMode(const char* mode)
{
    OpArgs args;
    args.Set("mode", std::string(mode));
    InvokeOperator("render.set_view_mode", args);
}

/// operator をメニュー項目として描く
/// 表示名・ショートカット文字列・実行可否・実体の 4 つとも登録済みの情報から引く。
/// 直書きするとリバインドで表示だけが嘘になり、実行可否もキー側と別式になる。
bool EditorApp::MenuItemOp(const char* operatorId, const char* labelOverride)
{
    return MenuItemOpArgs(operatorId, OpArgs{}, labelOverride);
}

bool EditorApp::MenuItemOpArgs(const char* operatorId, const OpArgs& args,
                               const char* labelOverride)
{
    const EditorOperator* op = m_operators.Find(operatorId);
    if (op == nullptr) {
        FBZZ_LOG_WARN("MenuItemOp: 未登録の operator です (id=%s)", operatorId);
        return false;
    }

    /// @note 実際に割り当てられているキーを表示する (未割り当てなら表示なし)。
    std::string shortcut;
    if (const Hotkey* hk = m_hotkeys.FindByOperator(operatorId))
        shortcut = HotkeyManager::FormatBinding(*hk);

    /// @note 登録簿は英語のまま (op.list / AI バス / ホットキー保存の鍵になる)。訳すのは
    ///       描くときだけ。LOC は "訳###原文" を返すので、ID は英語版と同じままになる。
    const char* label = LOC((labelOverride != nullptr) ? labelOverride : op->label.c_str());
    const bool  enabled = CanInvokeOperator(operatorId, args);

    /// @note チェックマークも登録簿から引く。フラグを直接指すと表示と実体が別経路になり、
    ///       operator 側に条件を足してもメニューの見た目に反映されない。
    bool checked = false;
    if (op->checked) {
        const OpContext context = MakeOpContext();
        checked = op->checked(context, args);
    }

    if (!ImGui::MenuItem(label, shortcut.empty() ? nullptr : shortcut.c_str(), checked, enabled))
        return false;

    InvokeOperator(operatorId, args);
    return true;
}

/// Operator のカテゴリ文字列を、ショートカット一覧の見出し分類へ対応づける。
/// 一覧は enum、Operator 側は文字列カテゴリなので、対応表をここに 1 つ置く。
static HotkeyCategory HotkeyCategoryFromOperator(const std::string& category)
{
    if (category == "File")      return HotkeyCategory::File;
    if (category == "Edit")      return HotkeyCategory::Edit;
    if (category == "Selection") return HotkeyCategory::Selection;
    if (category == "Viewport")  return HotkeyCategory::Viewport;
    if (category == "Gizmo")     return HotkeyCategory::Gizmo;
    if (category == "Play")      return HotkeyCategory::Play;
    if (category == "Panels")    return HotkeyCategory::Panels;
    if (category == "Tools")     return HotkeyCategory::Tools;
    if (category == "Render")    return HotkeyCategory::Tools;
    return HotkeyCategory::Edit;
}

/// ホットキー登録 — キー割り当ての単一の定義場所
/// ここに書けば入力処理・F1 の一覧・リバインド UI・設定への永続化が全部ついてくる
/// (パネル側で IsKeyPressed を直接叩くと、その 4 つが揃わない)。
/// 「何をするか」「いつ実行できるか」は OperatorRegistry が持ち、ここは operator id に
/// キーを割り当てるだけの表。条件式を各面へ写すと必ずずれる。
/// Docs/design/editor-operator-model.md
void EditorApp::RegisterDefaultHotkeys()
{
    /// @note 説明専用エントリ (RegisterInfo) で使う
    using Cat   = HotkeyCategory;
    using Scope = HotkeyScope;

    /// @note operator id へキーを割り当てる。表示名・分類・実行・実行可否はすべて
    ///       レジストリ側から導出するので、ここでキー以外を書くことはない。
    auto bind = [this](const char* operatorId, int key, bool ctrl, bool shift, bool alt,
                       Scope scope) {
        const EditorOperator* op = m_operators.Find(operatorId);
        if (op == nullptr) {
            FBZZ_LOG_WARN("RegisterDefaultHotkeys: 未登録の operator です (id=%s)", operatorId);
            return;
        }

        Hotkey hk;
        hk.name       = op->label;
        hk.operatorId = op->id;
        hk.imguiKey   = key;
        hk.ctrl       = ctrl;
        hk.shift      = shift;
        hk.alt        = alt;
        hk.scope      = scope;
        hk.category   = HotkeyCategoryFromOperator(op->category);

        const std::string id = op->id;
        hk.callback = [this, id]() { InvokeOperator(id); };
        hk.enabled  = [this, id]() { return CanInvokeOperator(id); };

        m_hotkeys.Register(std::move(hk));
    };

    /// @note Scene View と Hierarchy のどちらにフォーカスがあっても効く編集操作。
    ///       Unity 同様、選択後にフォーカスを移さず Delete / Ctrl+D を通す。
    constexpr Scope kEditScopes = Scope::SceneViewport | Scope::Hierarchy;

    /// @name File
    bind("scene.new",      ImGuiKey_N, true, false, false, Scope::Global);
    bind("scene.open",     ImGuiKey_O, true, false, false, Scope::Global);
    bind("scene.save",     ImGuiKey_S, true, false, false, Scope::Global);
    bind("scene.save_as",  ImGuiKey_S, true, true,  false, Scope::Global);

    /// @name Edit
    bind("edit.undo", ImGuiKey_Z, true, false, false, Scope::Global);
    bind("edit.redo", ImGuiKey_Y, true, false, false, Scope::Global);
    /// @note Ctrl+Shift+Z は Unity / Photoshop 系の Redo。Ctrl+Y と併存させる。
    ///       同じ operator への 2 本目なので operatorId は付けない (付けると Rebind が
    ///       id で引いたときどちらを指すか決まらない)。保存鍵は表示名になる。
    {
        const EditorOperator* redo = m_operators.Find("edit.redo");
        if (redo != nullptr) {
            Hotkey hk;
            hk.name     = "Redo (Alt)";
            hk.imguiKey = ImGuiKey_Z;
            hk.ctrl     = true;
            hk.shift    = true;
            hk.scope    = Scope::Global;
            hk.category = HotkeyCategory::Edit;
            hk.callback = [this]() { InvokeOperator("edit.redo"); };
            hk.enabled  = [this]() { return CanInvokeOperator("edit.redo"); };
            m_hotkeys.Register(std::move(hk));
        }
    }

    bind("edit.delete_selected", ImGuiKey_Delete, false, false, false, kEditScopes);
    bind("edit.duplicate",       ImGuiKey_D, true, false, false, kEditScopes);
    bind("edit.copy",            ImGuiKey_C, true, false, false, kEditScopes);
    bind("edit.paste",           ImGuiKey_V, true, false, false, kEditScopes);
    bind("edit.paste_as_child",  ImGuiKey_V, true, true,  false, kEditScopes);
    bind("edit.rename",          ImGuiKey_F2, false, false, false, Scope::Hierarchy);
    bind("node.create_empty",    ImGuiKey_N, true, true,  false, kEditScopes);

    /// @name Selection
    bind("select.all",     ImGuiKey_A, true, false, false, kEditScopes);
    bind("select.clear",   ImGuiKey_Escape, false, false, false, kEditScopes);
    bind("select.back",    ImGuiKey_LeftArrow,  false, false, true, Scope::Global);
    bind("select.forward", ImGuiKey_RightArrow, false, false, true, Scope::Global);

    /// @name Viewport
    bind("view.frame_selected", ImGuiKey_F, false, false, false, Scope::SceneViewport);

    /// @note Alt+数字: 素の 1~9 はカメラブックマーク、Shift+数字はその保存で埋まっているため Alt を使う。
    ///       ViewportPanel のブックマーク処理も Alt 押下中はスキップし、取り合いを避けている。
    bind("view.toggle_projection", ImGuiKey_O, false, false, false, Scope::SceneViewport);
    bind("view.axis_front",  ImGuiKey_1, false, false, true, Scope::SceneViewport);
    bind("view.axis_back",   ImGuiKey_2, false, false, true, Scope::SceneViewport);
    bind("view.axis_left",   ImGuiKey_3, false, false, true, Scope::SceneViewport);
    bind("view.axis_right",  ImGuiKey_4, false, false, true, Scope::SceneViewport);
    bind("view.axis_top",    ImGuiKey_5, false, false, true, Scope::SceneViewport);
    bind("view.axis_bottom", ImGuiKey_6, false, false, true, Scope::SceneViewport);

    /// @name Gizmo
    bind("gizmo.move",         ImGuiKey_W, false, false, false, Scope::SceneViewport);
    bind("gizmo.rotate",       ImGuiKey_E, false, false, false, Scope::SceneViewport);
    bind("gizmo.scale",        ImGuiKey_R, false, false, false, Scope::SceneViewport);
    bind("gizmo.toggle_space", ImGuiKey_Q, false, false, false, Scope::SceneViewport);
    bind("gizmo.toggle_pivot", ImGuiKey_Z, false, false, false, Scope::SceneViewport);
    bind("gizmo.toggle_snap",  ImGuiKey_X, false, false, false, Scope::SceneViewport);

    /// @name Play
    bind("play.toggle", ImGuiKey_P, true, false, false, Scope::Global);
    bind("play.pause",  ImGuiKey_P, true, true,  false, Scope::Global);

    /// @name Panels
    /// @note MenuItemOp が実割り当てから表示を引くので、「表示だけあるショートカット」は成立しない。
    bind("tools.build_settings", ImGuiKey_B, true, true, false, Scope::Global);

    bind("panel.command_palette", ImGuiKey_K, true, false, false, Scope::Global);
    bind("panel.shortcut_list",   ImGuiKey_F1, false, false, false, Scope::Global);

    /// @name 説明専用エントリ
    /// @note マウス操作や数字キー列はキー 1 つに割り当てられないが、一覧としては同じくらい要る。
    ///       別表に切り出すとそこがまた二重管理になるので、同じ器に入れて一覧を 1 本に保つ。
    m_hotkeys.RegisterInfo("Look around",     "RMB drag",       Cat::Viewport, Scope::SceneViewport);
    m_hotkeys.RegisterInfo("Fly (while RMB)", "W / A / S / D / Q / E", Cat::Viewport, Scope::SceneViewport);
    m_hotkeys.RegisterInfo("Pan",             "MMB drag",       Cat::Viewport, Scope::SceneViewport);
    m_hotkeys.RegisterInfo("Orbit",           "Alt + LMB drag", Cat::Viewport, Scope::SceneViewport);
    m_hotkeys.RegisterInfo("Zoom",            "Mouse wheel",    Cat::Viewport, Scope::SceneViewport);
    m_hotkeys.RegisterInfo("Camera bookmark: save / recall",
                           "Shift+1~9 / 1~9", Cat::Viewport, Scope::SceneViewport);
    m_hotkeys.RegisterInfo("Multi-select",    "Ctrl + click",   Cat::Selection, Scope::SceneViewport);
    m_hotkeys.RegisterInfo("Box select",      "LMB drag on empty space",
                           Cat::Selection, Scope::SceneViewport);
    m_hotkeys.RegisterInfo("Momentary grid snap", "Hold Ctrl while dragging a gizmo",
                           Cat::Gizmo, Scope::SceneViewport);
    m_hotkeys.RegisterInfo("Vertex snap",     "Hold V + drag",  Cat::Gizmo, Scope::SceneViewport);
    m_hotkeys.RegisterInfo("Surface snap",    "Ctrl + Shift + drag", Cat::Gizmo, Scope::SceneViewport);

    /// @note Fluid Editor はキーをパネル内で直接拾う (対象が «開いている文書» でありレジストリの
    ///       operator にできない)。入力には関与しない説明専用エントリとして一覧にだけ出す —
    ///       一覧に出ないキーは «無い» のと同じで、特に Ctrl+S はここで明示しないと意味が読めない。
    m_hotkeys.RegisterInfo("Save fluid document", "Ctrl+S", Cat::File, Scope::FluidEditor);
    m_hotkeys.RegisterInfo("Play / pause preview", "Space", Cat::Play, Scope::FluidEditor);
    m_hotkeys.RegisterInfo("Step frame",           "Left / Right", Cat::Play, Scope::FluidEditor);
    m_hotkeys.RegisterInfo("Jump to start / end",  "Home / End",   Cat::Play, Scope::FluidEditor);
    m_hotkeys.RegisterInfo("Delete selected part", "Delete",       Cat::Edit, Scope::FluidEditor);
    m_hotkeys.RegisterInfo("Insert motion key at playhead", "K",   Cat::Edit, Scope::FluidEditor);
    m_hotkeys.RegisterInfo("Rename selected part", "F2",           Cat::Edit, Scope::FluidEditor);

    /// @note 保存済みリバインドの適用は EditorApp::OpenProject が行う (設定を読むのがそこ)。
    ///       Rebind は既定値を全部積んだ後でないと対象を引けないので、順序はここより後。

    /// @note scope の判定は EditorContext のフォーカス状態から答える。パネルは描画中にしか
    ///       自身のフォーカスを知れないため、ここで見るのは 1 フレーム前の状態になる
    ///       (キー入力への応答としては問題ない)。
    m_hotkeys.SetScopeResolver([this](HotkeyScope scope) {
        /// @note 「今フォーカスされている面」は 1 つ (EditorContext::focusedPanelScope)。
        ///       パネルが増えてもここは変わらない — 各パネルが IPanel::GetHotkeyScope() で名乗る。
        if (m_ctx.PanelScopeFocused(scope)) return true;

        /// @note Scene View だけはホバーでも効かせる。ギズモ切替やカメラ操作は «絵を見ながら»
        ///       押すもので、先にクリックしてフォーカスを取る手順を挟むと手が止まる。
        ///       他の面はフォーカスのみで判定する方が読める。
        return HasScope(scope, HotkeyScope::SceneViewport) && m_ctx.sceneViewportHovered;
    });
}

}
