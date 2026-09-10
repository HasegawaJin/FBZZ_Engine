/// @file    EditorApp_MenuBar.cpp
/// @brief   メインメニューバーの構築とホットキー登録。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// WHY: メニューバーは ImGui の MenuItem 呼び出しが大量に並ぶ UI 記述コードであり、
/// ライフサイクル管理やシーン I/O とは関心が異なる。
/// 独立ファイルに分離することで、メニュー項目の追加・変更を局所化できる。
#include <Editor/EditorApp.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Panels/AiSettingsPanel.hpp>
#include <Editor/Panels/AssetMaintenancePanel.hpp>
#include <Editor/Panels/IblBakePanel.hpp>
#include <Editor/Panels/NavigationPanel.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/HotkeyManager.hpp>
#include <Editor/Util/Localization.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
#include <Editor/Util/Toast.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Scene/ScriptValidation.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Cursor.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Audio/AudioManager.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>
#include <algorithm>
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
    Reload
};

void DrawPlayToolbarIcon(PlayToolbarIcon icon, const ImVec2& min, const ImVec2& max, ImU32 color)
{
    // WHY: PlayMode のアイコンは短い記号テキストだと幅やフォントに左右され、エディターの工具感が弱くなる。
    // WHAT: ImGui の DrawList で単純な幾何形状を描き、フォント非依存の固定アイコンとして表示する。
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImDrawListFlags oldFlags = drawList->Flags;
    drawList->Flags |= ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AntiAliasedLines;

    auto drawTriangle = [drawList, color](const ImVec2& a, const ImVec2& b, const ImVec2& c) {
        // WHY: 塗りつぶし三角形だけだと斜辺のジャギーが目立つため、
        //      同色のアンチエイリアス線を重ねて輪郭をなじませる。
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
    case PlayToolbarIcon::Reload: {
        // 円弧 (約 300°) + 先端に矢頭
        constexpr float kPi        = 3.14159265f;
        constexpr float r          = 5.5f;
        constexpr float startAngle = kPi * 0.25f;
        constexpr float endAngle   = startAngle + kPi * 1.67f;
        drawList->PathArcTo(center, r, startAngle, endAngle, 16);
        drawList->PathStroke(color, false, 1.8f);

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
    const ImVec2& size)
{
    // WHY: PlayMode 操作は Godot のように常に同じ位置へ置き、状態確認と操作を視線移動なしで行えるようにする。
    // WHAT: active 時だけ操作種別の色を背景へ乗せ、無効時は ImGui の Disabled スタイルで入力も止める。
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
    const ImU32 iconColor = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    DrawPlayToolbarIcon(icon, min, max, iconColor);

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);

    if (!enabled)
        ImGui::EndDisabled();

    if (active)
        ImGui::PopStyleColor(3);

    return enabled && pressed;
}

} // namespace

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
    constexpr uint16_t TOGGLE_GRID     = 401;
    constexpr uint16_t TOGGLE_LIGHTS   = 402;
    constexpr uint16_t TOGGLE_VFX      = 403;
    constexpr uint16_t TOGGLE_SKELETON = 404;
    constexpr uint16_t TOGGLE_STATS    = 405;
    constexpr uint16_t TOGGLE_HOTRELOAD = 406;
    constexpr uint16_t TOGGLE_COLLIDERS = 407;
    constexpr uint16_t TOGGLE_TERRAIN_COLLISION = 408;
    constexpr uint16_t TOGGLE_NAVMESH = 409;
    constexpr uint16_t TOGGLE_AI_SENSORS = 414;
    constexpr uint16_t TOGGLE_DECAL_BOUNDS = 415;
    constexpr uint16_t TOGGLE_SHADOW = 422;
    constexpr uint16_t VIEW_LIT        = 410;
    constexpr uint16_t VIEW_UNLIT      = 411;
    constexpr uint16_t VIEW_WIRE_LIT   = 412;
    constexpr uint16_t VIEW_WIRE_UNLIT = 413;
    constexpr uint16_t TOGGLE_MAP       = 501;
    constexpr uint16_t OPEN_BUILD       = 502;
    constexpr uint16_t OPEN_IBL         = 503;
    constexpr uint16_t OPEN_NAVIGATION  = 504;
    constexpr uint16_t OPEN_ASSET_MAINT = 505;
    constexpr uint16_t OPEN_AI_SETTINGS = 600;
    constexpr uint16_t PANEL_BASE       = 1000;

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
    terrainTools.push_back(command("Water Tool", 511));

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
    menus.push_back(submenu("Play", {
        command("Play", PLAY), command("Stop", STOP), command("Pause", PAUSE),
        command("Step", STEP), separator(), command("Reload Scripts", RELOAD_SCRIPTS)
    }));
    menus.push_back(submenu("View", {
        submenu("Panels", std::move(panels)),
        command("Reset UI Scale", 700)
    }));
    menus.push_back(submenu("Debug", {
        command("Analysis", OPEN_ANALYSIS), separator(),
        command("Grid", TOGGLE_GRID), command("Light Range", TOGGLE_LIGHTS),
        command("VFX Force Fields / Emitters", TOGGLE_VFX),
        command("Skeleton", TOGGLE_SKELETON), command("Stats", TOGGLE_STATS),
        command("Colliders", TOGGLE_COLLIDERS), command("Terrain Collision", TOGGLE_TERRAIN_COLLISION),
        command("NavMesh", TOGGLE_NAVMESH), command("AI Sensors", TOGGLE_AI_SENSORS),
        command("Decal Bounds", TOGGLE_DECAL_BOUNDS),
        command("Hot Reload", TOGGLE_HOTRELOAD), separator(),
        command("Shadow", TOGGLE_SHADOW),
        submenu("View Mode", std::move(debugViewMode))
    }));
    menus.push_back(submenu("Tools", {
        command("Map Editing Mode", TOGGLE_MAP),
        submenu("Terrain & Map", std::move(terrainTools)),
        command("Build Settings...", OPEN_BUILD), command("IBL Baker...", OPEN_IBL),
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

        // ネイティブメニューは項目ごとの有効/無効を持たないが、InvokeOperator は poll を
        // 満たさない要求を拒否するので、グレーアウトできない面でも同じ条件が効く。
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
        case PLAY:            InvokeOperator("play.start"); break;
        case STOP:            InvokeOperator("play.stop"); break;
        case PAUSE:           InvokeOperator("play.pause"); break;
        case STEP:            InvokeOperator("play.step"); break;
        case RELOAD_SCRIPTS:  InvokeOperator("script.reload"); break;
        case OPEN_ANALYSIS:   InvokeOperator("tools.analysis"); break;
        // 表示トグルとビューモードは Operator を通す。同じ切り替えを 3 面が持つので、
        // 書き込み先 (EditorContext か ProjectSettings か) を各面が覚えていると必ずずれる。
        case TOGGLE_GRID:      InvokeOperator("render.show_grid"); break;
        case TOGGLE_LIGHTS:    InvokeOperator("render.show_light_range"); break;
        case TOGGLE_VFX:       InvokeOperator("render.show_vfx_gizmos"); break;
        case TOGGLE_SKELETON:  InvokeOperator("render.show_skeleton"); break;
        case TOGGLE_STATS:     InvokeOperator("render.show_stats"); break;
        case TOGGLE_HOTRELOAD: InvokeOperator("debug.hot_reload"); break;
        case TOGGLE_COLLIDERS: InvokeOperator("render.show_colliders"); break;
        case TOGGLE_TERRAIN_COLLISION: InvokeOperator("render.show_terrain_collision"); break;
        case TOGGLE_NAVMESH:    InvokeOperator("render.show_navmesh"); break;
        case TOGGLE_AI_SENSORS: InvokeOperator("render.show_nav_sensors"); break;
        case TOGGLE_DECAL_BOUNDS: InvokeOperator("render.show_decal_bounds"); break;
        case TOGGLE_SHADOW:  InvokeOperator("render.shadow_enabled"); break;
        case VIEW_LIT:       InvokeViewMode("lit"); break;
        case VIEW_UNLIT:     InvokeViewMode("unlit"); break;
        case VIEW_WIRE_LIT:  InvokeViewMode("wireframe_lit"); break;
        case VIEW_WIRE_UNLIT:InvokeViewMode("wireframe_unlit"); break;
        case 700:            InvokeOperator("view.reset_ui_scale"); break;
        case TOGGLE_MAP:     InvokeOperator("tools.map_editing_mode"); break;
        case OPEN_BUILD:     InvokeOperator("tools.build_settings"); break;
        // パネルを前面に出すのは panel.focus 1 つで足りる。パネルごとに
        // operator を生やすと m_panels という単一の出所が二重管理へ戻る。
        case OPEN_IBL:         InvokePanelFocus(m_iblBakePanel); break;
        case OPEN_NAVIGATION:  InvokePanelFocus(m_navigationPanel); break;
        case OPEN_ASSET_MAINT: InvokePanelFocus(m_assetMaintenancePanel); break;
        case OPEN_AI_SETTINGS: InvokePanelFocus(m_aiSettingsPanel); break;
        case 510: InvokeOperator("tools.terrain"); break;
        case 511: InvokeOperator("tools.water"); break;
        default: return false;
        }
        return true;
    });
}

void EditorApp::BuildMenuBar(EditorContext& ctx)
{
    if (!ImGui::BeginMenuBar()) return;

    // FBZZ Studio のワークスペースであることを常時示すブランドマーク。
    // WHY: OS タイトルバーを隠す最大化・マルチビューポート環境でも製品識別を失わない。
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

    // --- File ------------------------------------------------------------
    if (ImGui::BeginMenu(LOC("File"))) {
        // Prefab 編集モード中はシーン操作を伏せ、対象がアセットであることを明示する。
        // WHY: 項目名が "Save" のままだと、何が保存されるのかが読み取れない。
        const bool inPrefabEdit = ctx.InPrefabEditMode();
        MenuItemOp("scene.new");
        MenuItemOp("scene.open");
        MenuItemOp("scene.save", inPrefabEdit ? "Save Prefab" : nullptr);
        MenuItemOp("scene.save_as");
        // Prefab 編集中だけ出す。poll (prefab.close) も同じ条件を持つので、
        // 表示していない状況では AI からも通らない。
        if (inPrefabEdit) MenuItemOp("prefab.close");
        MenuItemOp("asset.save_all");
        ImGui::Separator();
        MenuItemOp("app.exit");
        ImGui::EndMenu();
    }

    // --- Edit ------------------------------------------------------------
    if (ImGui::BeginMenu(LOC("Edit"))) {
        // 直前の操作名を添えて「何が戻るのか」を読めるようにする。
        // ラベルだけが動的で、実行可否と実体は operator 側にある。
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

    // --- View ------------------------------------------------------------
    if (ImGui::BeginMenu(LOC("View"))) {
        // panel.set_visible の投影にすることで、人が押すのと同じ実体を AI も呼べる
        // (パネルが増えても operator は 1 つのまま)。
        if (ImGui::BeginMenu(LOC("Panels"))) {
            for (auto& panel : m_panels) {
                if (!panel->ShowInViewMenu()) continue;
                OpArgs args;
                args.Set("panel", std::string(panel->GetWindowName()));
                MenuItemOpArgs("panel.set_visible", args, panel->GetViewMenuName());
            }
            ImGui::EndMenu();
        }

        // Scene View の視点。軸ビューはナビゲーションギズモのクリックと同じ操作を指す。
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

        // UI 全体スケール (フォント + 余白)。設定に永続化される。
        // スライダー自体は連続値のドラッグなので operator には乗らないが、
        // 適用は view.set_ui_scale を通す (範囲の宣言と適用処理を 1 箇所に保つ)。
        ImGui::Separator();
        ImGui::TextDisabled("%s", LOCT("UI Scale"));
        // 範囲は operator の params 宣言から引く。スライダー側に直書きすると、
        // 人は 0.5x にできるのに AI からは BAD_ARG で弾かれる (同じ操作の限界が
        // 面ごとに違う) という、この設計が消したいずれが範囲という形で再発する。
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

    // --- Debug -----------------------------------------------------------
    if (ImGui::BeginMenu(LOC("Debug"))) {
        // WHY: Godot は表示パネル操作とデバッグ描画切替を別メニューに分けている。
        //      FBZZ でも View はレイアウト・パネル、Debug は実行/描画診断に寄せることで項目の意味を読み取りやすくする。
        MenuItemOp("tools.analysis", "Analysis");
        ImGui::Separator();
        // 全項目を operator の投影にする。フラグのアドレスを直接渡すと、同じフラグを
        // 切り替える operator と表示が別経路になり、AI からも見えない。
        // --- Scene Overlays ---
        MenuItemOp("render.show_grid",        "Grid");
        MenuItemOp("render.show_light_range", "Light Range");
        MenuItemOp("render.show_vfx_gizmos",  "VFX Force Fields / Emitters");
        MenuItemOp("render.show_skeleton",    "Skeleton");
        MenuItemOp("render.show_stats",       "Stats");
        ImGui::Separator();
        // --- Physics / Rendering ---
        MenuItemOp("render.show_colliders",         "Colliders");
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
        ImGui::Separator();
        // --- Culling ---
        // Scene View はデバッグカメラで描くため CameraComponent の設定が届かない。
        // 「消えた原因がカリングか」を切り分ける唯一の口なので Debug 側へ出す。
        MenuItemOp("render.scene_view_occlusion_culling", "Scene View Occlusion Culling");
        ImGui::Separator();
        // --- Tools ---
        MenuItemOp("debug.hot_reload", "Hot Reload");
        ImGui::Separator();
        if (ImGui::BeginMenu(LOC("View Mode"))) {
            // 排他選択は 1 つの operator に引数で渡す。チェックは checked を
            // 同じ引数で評価した値なので、「表示は Lit なのに実体は Unlit」が作れない。
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
        // WHY Bloom / FXAA のトグルが無いか: ポストプロセスの所有者は
        //     Post Process Volume + Post Process Profile (.fzdata) へ一本化した。
        //     Shadow はプロジェクト全体の描画構成なのでここに残す。
        MenuItemOp("render.shadow_enabled", "Shadow");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(LOC("Tools"))) {
        MenuItemOp("tools.map_editing_mode");
        ImGui::Separator();
        if (ImGui::BeginMenu(LOC("Terrain & Map"))) {
            MenuItemOp("tools.terrain");
            MenuItemOp("tools.water");
            ImGui::EndMenu();
        }
        MenuItemOp("tools.build_settings");
        ImGui::Separator();
        // パネルごとに operator を生やすと m_panels という単一の出所が二重管理へ戻る。
        // 名前を引数で渡す 1 つの操作で足りる。
        if (m_iblBakePanel && ImGui::MenuItem(LOC("IBL Baker...")))
            InvokePanelFocus(m_iblBakePanel);
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
        // チェックマークは operator の checked (= 実際の待受状態) なので、
        // 状態表示の役割を保ったままその場で切り替えられる。
        MenuItemOp("ai.command_bus", "Editor Command Bus");
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

void EditorApp::BuildPlayToolbar(EditorContext& ctx)
{
    // WHY: Play 系操作を上部中央へ独立配置し、メニュー項目より実行状態を読み取りやすくする。
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

    const float toolbarButtonY = ImGui::GetCursorPosY();
    // 実行可否は operator の poll から引く。以前はここだけが持っていた条件
    // (scriptReloadBusy 中は Play 不可) がホットキーとパレットに無く、
    // コンパイル中でも Ctrl+P で Play へ入れてしまっていた。
    const bool canToggleMapMode = CanInvokeOperator("tools.map_editing_mode");
    if (ctx.mapEditingMode)
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(ThemeColor::Secondary));
    if (!canToggleMapMode)
        ImGui::BeginDisabled();
    if (ImGui::Button(ctx.mapEditingMode ? "EXIT MAP" : "MAP MODE", { 92.0f, 24.0f }))
        InvokeOperator("tools.map_editing_mode");
    if (!canToggleMapMode)
        ImGui::EndDisabled();
    if (ctx.mapEditingMode)
        ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        if (!canToggleMapMode)
            ImGui::SetTooltip("Open a scene to enable Map Editing Mode");
        else if (ctx.mapEditingMode)
            ImGui::SetTooltip("Exit Map Editing Mode — restores normal editor layout");
        else
            ImGui::SetTooltip("Enter Map Editing Mode\nTerrain / Water tools in a focused layout");
    }

    const float groupWidth = BUTTON_SIZE.x * 5.0f + BUTTON_SPACING * 4.0f;
    // ここでは Map ボタンを描いた後でも、子 Window 全体の幅を基準にする。
    // GetContentRegionAvail() は現在カーソル位置からの残幅なので、Map ボタン分だけ
    // 二重に引かれて Play 群が左へ寄り、右端が欠ける原因になる。
    const float availableWidth = ImGui::GetWindowWidth();
    // Mapボタンの領域を予約してから中央寄せする。以前はツールバー全体の中央を
    // 基準にしていたため、ウィンドウ幅が一時的に狭くなると MAP MODE と Play 群が重なった。
    const float playAreaLeft = 8.0f + 92.0f + 16.0f;
    const float centerOffset = (std::max)(playAreaLeft, (availableWidth - groupWidth) * 0.5f);
    ImGui::SetCursorPos({ centerOffset, toolbarButtonY });
    const bool scriptReloadBusy =
        ctx.scriptReloadBusy ||
        ctx.hotReloadState == EditorContext::HotReloadState::Compiling ||
        ctx.hotReloadState == EditorContext::HotReloadState::Reloading;
    const char* playTooltip = scriptReloadBusy
        ? "Scripts are compiling/reloading..."
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
        const char* reloadTooltip = scriptReloadBusy ? "Scripts are compiling..." : "Reload Scripts";
        if (PlayToolbarButton(
            "##ScriptReload",
            reloadTooltip,
            PlayToolbarIcon::Reload,
            CanInvokeOperator("script.reload"),
            scriptReloadBusy,
            reloadColor,
            BUTTON_SIZE))
            InvokeOperator("script.reload");
    }

    // ── 右端: ホットリロードステータス / PLAYING ラベル ────────────────────
    {
        // ホットリロードステータステキストを決定する
        const char* reloadText  = nullptr;
        ImVec4      reloadColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        switch (ctx.hotReloadState) {
        case EditorContext::HotReloadState::Compiling:
            reloadText  = "Compiling...";
            reloadColor = { 1.0f, 0.85f, 0.2f,  1.0f };
            break;
        case EditorContext::HotReloadState::Reloading:
            reloadText  = "Reloading...";
            reloadColor = { 0.5f, 0.8f,  1.0f,  1.0f };
            break;
        case EditorContext::HotReloadState::Done:
            reloadText  = "Reload OK";
            reloadColor = { 0.35f, 1.0f, 0.45f, 1.0f };
            break;
        case EditorContext::HotReloadState::Failed:
            reloadText  = "Compile Error";
            reloadColor = { 1.0f, 0.35f, 0.35f, 1.0f };
            break;
        default: break;
        }

        // PLAYING / PAUSED ラベル
        const char*  playLabel    = !isEditor ? (isPlaying ? "PLAYING" : "PAUSED") : nullptr;
        const ImVec4 playLabelCol = isPlaying
            ? ImVec4{ 0.28f, 0.88f, 0.53f, 1.0f }
            : ImVec4{ 0.92f, 0.72f, 0.28f, 1.0f };

        // 右端からテキスト幅で逆算して配置する (描画対象がある場合のみ)
        constexpr float kGap = 6.0f;
        float totalW = 0.0f;
        if (reloadText) totalW += ImGui::CalcTextSize(reloadText).x + kGap;
        if (playLabel)  totalW += ImGui::CalcTextSize(playLabel).x  + kGap;

        if (totalW > 0.0f) {
            const float groupRight = centerOffset + groupWidth;
            const float posX = availableWidth - totalW - 8.0f;
            const float posY = (TOOLBAR_HEIGHT - ImGui::GetTextLineHeight()) * 0.5f;
            // ステータス文字をボタン群へ重ねず、狭いフレームでは表示を省略する。
            // WHY: Compiling / PLAYING の表示が Play ボタンを押しつぶして欠けるより、
            //      操作ボタンを常に完全表示する方が安全である。
            if (posX > groupRight + kGap) {
                if (posX > ImGui::GetCursorPosX())
                    ImGui::SetCursorPos({ posX, posY });

                if (reloadText) {
                    ImGui::PushStyleColor(ImGuiCol_Text, reloadColor);
                    ImGui::TextUnformatted(reloadText);
                    ImGui::PopStyleColor();
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

// =============================================================================
// ビルド失敗通知バー
// =============================================================================

// WHY: 従来はビルド失敗が StatusBar に数秒表示されて消えるだけで見落としやすかった。
//      失敗が残っている間、ツールバー直下に消えない赤帯を出し、[Show] で該当エラーへ、
//      [Dismiss] で明示的に閉じられるようにする。成功ビルドで自動的に消える。
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

    // 右寄せで操作ボタンを置く。
    const float btnW = 150.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - btnW);
    if (ImGui::SmallButton("Show")) {
        ctx.requestFocusBuildError = true;   // Build Output を開いて最初のエラーへスクロール
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
    // 閉じた後に «増えた» ときだけ出し直す。同じ件数のまま出し続けない。
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

// =============================================================================
// ホットキー登録
// =============================================================================

void EditorApp::StartPlayMode()
{
    // WHY: Play ツールバーボタンと Ctrl+P ホットキーの共通経路。
    //      ガード条件をここに集約し、どの入力経路でも同じ前提チェックを通す。
    const bool scriptReloadBusy =
        m_ctx.scriptReloadBusy ||
        m_ctx.hotReloadState == EditorContext::HotReloadState::Compiling ||
        m_ctx.hotReloadState == EditorContext::HotReloadState::Reloading;
    if (!m_ctx.activeScene || !m_playMode.IsInEditor() || scriptReloadBusy)
        return;
    // Prefab 編集面には「シーン」が無い (カメラもライトも無い、プレファブ単体)。
    // WHY: そのまま Play すると空舞台でゲームが動き出し、Stop 時のスナップショット復元も
    //      プレファブの内容に対して行われる。編集面から出るまで開始させない。
    if (m_ctx.InPrefabEditMode()) {
        FBZZ_LOG_WARN("Play: close the prefab edit mode first");
        return;
    }

    // FBZZ_REQUIRE_COMPONENT の充足をシーン全体でまとめて検証する。
    // 付け忘れは「動かないけどエラーも出ない」形でしか現れないので、Play を押した瞬間に
    // Console へ全件出す。ただし Play は止めない — 作りかけを走らせるのが Play の役目。
    if (const auto issues = scene::ValidateSceneScriptRequirements(*m_ctx.activeScene);
        !issues.empty()) {
        for (const auto& issue : issues)
            FBZZ_LOG_ERROR("Script requirement: %s",
                           scene::FormatScriptRequirementIssue(issue).c_str());
        Toast::Error(std::to_string(issues.size()) +
                     " missing script component(s) — see Console");
    }

    m_undoStack.Clear();
    // 編集中に何かが基底を書いていても、Play は «誰も要求していない» 状態から始める。
    // WHY ここで畳むか: UpdatePlayCursorControls はフレーム先頭で、Play を押した次の
    //     フレームには既にスクリプトの OnStart が要求を積んでいる。あちらで畳むと
    //     名乗ったばかりの要求を消してしまう。
    core::Cursor::ClearRequests();
    // カーソルの絵もここで読む。スクリプトの OnStart は «OS カーソルの絵があるか» で
    // 自前のポインターを出すかどうかを決めるので、走り出す前に揃っている必要がある。
    m_ctx.projectSettings.cursor.Apply(m_ctx.projectRoot);
    RemoveEditorHiding();  // Play 前に editor-only 非表示を一時解除（スナップショットに active 状態で含める）
    // navMesh は TOML に保存されないため、Play 開始前にキャッシュしておく。
    // Stop 後の scene 復元で needsBake=true が立っても再ベイクせずに済む。
    m_navMeshPlayCache.clear();
    for (scene::EntityID eid : m_ctx.activeScene->GetEntities<scene::NavMeshSurfaceComponent>()) {
        auto* surf = m_ctx.activeScene->GetComponent<scene::NavMeshSurfaceComponent>(eid);
        auto* go   = m_ctx.activeScene->GetGameObject(eid);
        if (!surf || !go || !surf->navMesh.IsValid()) continue;
        NavMeshPlayCacheEntry entry;
        entry.navMesh    = surf->navMesh;
        entry.stats      = surf->bakeStats;
        entry.sourceHash = surf->bakedSourceHash;
        // ボクセル格子は Scene View の Voxels 表示専用で、Play 中は表示自体が抑止される。
        // 数 MB を二重に抱えないよう、預けている間は Surface から外す。
        entry.debug      = std::move(surf->bakeDebug);
        surf->bakeDebug  = {};
        m_navMeshPlayCache[go->instanceId] = std::move(entry);
    }
    // WHY: ScriptProxy は ScriptRuntime 経由でサブシステムを参照する。
    //      エディタは共通ProjectRuntimeのSceneManagerをUpdateするため、Play開始時に
    //      ScriptRuntime をオーバーライドして正しい参照先を指す。
    m_runtime.ActivateScriptRuntime(
        core::Application::Get().GetRenderer(),
        static_cast<uint32_t>(m_ctx.gameViewportWidth),
        static_cast<uint32_t>(m_ctx.gameViewportHeight)
    );
    // graphics プロキシの書き換え先を Play 中だけ開ける。実体は ProjectSettings::render で
    // 終了時に toml へ保存されるので、スナップショットを取らないと Play 中の変更が焼き付く。
    m_renderSettingsPlaySnapshot = m_ctx.projectSettings.render;
    core::Application::Get().SetActiveRenderSettings(&m_ctx.projectSettings.render);
    // Play 中の増加も Stop 後の残りも、この 1 つの基準から測る。
    if (m_ctx.resources != nullptr) {
        m_memoryLeakDiff.CaptureBaseline(*m_ctx.resources, "Play");
        // 最初の Play だけ «セッション基準» も置く。キャッシュの初回充填と、
        // 往復のたびに積むリークを、1 往復ぶんの差分だけでは区別できないため。
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
    // Play 中のスクリプトが変えた画質・明るさを編集側へ持ち込まない。
    // 選択状態だけは編集の続きなので、復元から外して現在のものを残す。
    core::Application::Get().SetActiveRenderSettings(nullptr);
    {
        auto selection = std::move(m_ctx.projectSettings.render.selectedObjects);
        m_ctx.projectSettings.render = m_renderSettingsPlaySnapshot;
        m_ctx.projectSettings.render.selectedObjects = std::move(selection);
    }
    // AudioSystemはSimOnlyのため、EditModeへ戻った後ではループVoiceを停止できない。
    // PauseではなくPlay終了時だけ一括停止し、BGMがEditor操作中まで残ることを防ぐ。
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

// 描画モードを operator へ渡す小さな補助 (ネイティブメニューの 4 項目が使う)。
void EditorApp::InvokeViewMode(const char* mode)
{
    OpArgs args;
    args.Set("mode", std::string(mode));
    InvokeOperator("render.set_view_mode", args);
}

// =============================================================================
// operator をメニュー項目として描く
// 表示名・ショートカット文字列・実行可否・実体の 4 つとも登録済みの情報から引く。
// 直書きするとリバインドで表示だけが嘘になり、実行可否もキー側と別式になる。
// =============================================================================
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

    // 実際に割り当てられているキーを表示する (未割り当てなら表示なし)。
    std::string shortcut;
    if (const Hotkey* hk = m_hotkeys.FindByOperator(operatorId))
        shortcut = HotkeyManager::FormatBinding(*hk);

    // 登録簿は英語のまま (op.list / AI バス / ホットキー保存の鍵になる)。訳すのは
    // 描くときだけ。LOC は "訳###原文" を返すので、ID は英語版と同じままになる。
    const char* label = LOC((labelOverride != nullptr) ? labelOverride : op->label.c_str());
    const bool  enabled = CanInvokeOperator(operatorId, args);

    // チェックマークも登録簿から引く。フラグを直接指すと表示と実体が別経路になり、
    // operator 側に条件を足してもメニューの見た目に反映されない。
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

// Operator のカテゴリ文字列を、ショートカット一覧の見出し分類へ対応づける。
// 一覧は enum、Operator 側は文字列カテゴリなので、対応表をここに 1 つ置く。
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

// =============================================================================
// ホットキー登録 — キー割り当ての単一の定義場所
// ここに書けば入力処理・F1 の一覧・リバインド UI・設定への永続化が全部ついてくる
// (パネル側で IsKeyPressed を直接叩くと、その 4 つが揃わない)。
// 「何をするか」「いつ実行できるか」は OperatorRegistry が持ち、ここは operator id に
// キーを割り当てるだけの表。条件式を各面へ写すと必ずずれる。
// Docs/design/editor-operator-model.md
// =============================================================================
void EditorApp::RegisterDefaultHotkeys()
{
    using Cat   = HotkeyCategory;   // 説明専用エントリ (RegisterInfo) で使う
    using Scope = HotkeyScope;

    // operator id へキーを割り当てる。表示名・分類・実行・実行可否はすべて
    // レジストリ側から導出するので、ここでキー以外を書くことはない。
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

    // Scene View と Hierarchy のどちらにフォーカスがあっても効く編集操作。
    // WHY: Unity では Scene View で選んだまま Delete / Ctrl+D が効く。
    //      Hierarchy へフォーカスを移さないと消せないのは動線として遠回り。
    constexpr Scope kEditScopes = Scope::SceneViewport | Scope::Hierarchy;

    // ── File ────────────────────────────────────────────────────────────────
    bind("scene.new",      ImGuiKey_N, true, false, false, Scope::Global);
    bind("scene.open",     ImGuiKey_O, true, false, false, Scope::Global);
    bind("scene.save",     ImGuiKey_S, true, false, false, Scope::Global);
    bind("scene.save_as",  ImGuiKey_S, true, true,  false, Scope::Global);

    // ── Edit ────────────────────────────────────────────────────────────────
    bind("edit.undo", ImGuiKey_Z, true, false, false, Scope::Global);
    bind("edit.redo", ImGuiKey_Y, true, false, false, Scope::Global);
    // Ctrl+Shift+Z は Unity / Photoshop 系の Redo。Ctrl+Y と併存させる。
    // 同じ operator への 2 本目なので operatorId は付けない (付けると Rebind が
    // id で引いたときどちらを指すか決まらない)。保存鍵は表示名になる。
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

    // ── Selection ───────────────────────────────────────────────────────────
    bind("select.all",     ImGuiKey_A, true, false, false, kEditScopes);
    bind("select.clear",   ImGuiKey_Escape, false, false, false, kEditScopes);
    bind("select.back",    ImGuiKey_LeftArrow,  false, false, true, Scope::Global);
    bind("select.forward", ImGuiKey_RightArrow, false, false, true, Scope::Global);

    // ── Viewport ────────────────────────────────────────────────────────────
    bind("view.frame_selected", ImGuiKey_F, false, false, false, Scope::SceneViewport);

    // WHY Alt + 数字か: 素の 1~9 はカメラブックマーク、Shift + 数字はその保存で埋まっている。
    //     Alt は「視点の作り方を変える」修飾として空いており、ViewportPanel の
    //     ブックマーク処理も Alt 押下中はスキップして取り合いを避けている。
    bind("view.toggle_projection", ImGuiKey_O, false, false, false, Scope::SceneViewport);
    bind("view.axis_front",  ImGuiKey_1, false, false, true, Scope::SceneViewport);
    bind("view.axis_back",   ImGuiKey_2, false, false, true, Scope::SceneViewport);
    bind("view.axis_left",   ImGuiKey_3, false, false, true, Scope::SceneViewport);
    bind("view.axis_right",  ImGuiKey_4, false, false, true, Scope::SceneViewport);
    bind("view.axis_top",    ImGuiKey_5, false, false, true, Scope::SceneViewport);
    bind("view.axis_bottom", ImGuiKey_6, false, false, true, Scope::SceneViewport);

    // ── Gizmo ───────────────────────────────────────────────────────────────
    bind("gizmo.move",         ImGuiKey_W, false, false, false, Scope::SceneViewport);
    bind("gizmo.rotate",       ImGuiKey_E, false, false, false, Scope::SceneViewport);
    bind("gizmo.scale",        ImGuiKey_R, false, false, false, Scope::SceneViewport);
    bind("gizmo.toggle_space", ImGuiKey_Q, false, false, false, Scope::SceneViewport);
    bind("gizmo.toggle_pivot", ImGuiKey_Z, false, false, false, Scope::SceneViewport);
    bind("gizmo.toggle_snap",  ImGuiKey_X, false, false, false, Scope::SceneViewport);

    // ── Play ────────────────────────────────────────────────────────────────
    bind("play.toggle", ImGuiKey_P, true, false, false, Scope::Global);
    bind("play.pause",  ImGuiKey_P, true, true,  false, Scope::Global);

    // ── Panels ──────────────────────────────────────────────────────────────
    // MenuItemOp が実割り当てから表示を引くので、「表示だけあるショートカット」は成立しない。
    bind("tools.build_settings", ImGuiKey_B, true, true, false, Scope::Global);

    bind("panel.command_palette", ImGuiKey_K, true, false, false, Scope::Global);
    bind("panel.shortcut_list",   ImGuiKey_F1, false, false, false, Scope::Global);

    // ── 説明専用エントリ ─────────────────────────────────────────────────────
    // マウス操作や数字キー列はキー 1 つに割り当てられないが、一覧としては同じくらい要る。
    // 別表に切り出すとそこがまた二重管理になるので、同じ器に入れて一覧を 1 本に保つ。
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

    // NOTE: 保存済みリバインドの適用は EditorApp::OpenProject が行う (設定を読むのがそこ)。
    //       Rebind は既定値を全部積んだ後でないと対象を引けないので、順序はここより後。

    // scope の判定は EditorContext のフォーカス状態から答える。
    // WHY: パネルは自分の描画中にしか自身のフォーカスを知れないため、
    //      ここで見るのは 1 フレーム前の状態になる。キー入力の応答としては問題ない。
    m_hotkeys.SetScopeResolver([this](HotkeyScope scope) {
        if (HasScope(scope, HotkeyScope::SceneViewport) &&
            (m_ctx.viewportFocused || m_ctx.sceneViewportHovered))
            return true;
        if (HasScope(scope, HotkeyScope::Hierarchy) && m_ctx.hierarchyFocused)
            return true;
        if (HasScope(scope, HotkeyScope::AssetBrowser) && m_ctx.assetBrowserFocused)
            return true;
        return false;
    });
}

} // namespace fbzz::editor
