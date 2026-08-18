// FBZZ Engine
// EditorApp_MenuBar.cpp | fbzz::editor
// メインメニューバーの構築とホットキー登録
//
// WHY: メニューバーは ImGui の MenuItem 呼び出しが大量に並ぶ UI 記述コードであり、
//      ライフサイクル管理やシーン I/O とは関心が異なる。
//      独立ファイルに分離することで、メニュー項目の追加・変更を局所化できる。
#include <Editor/EditorApp.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Panels/AiSettingsPanel.hpp>
#include <Editor/Panels/IblBakePanel.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/HotkeyManager.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
#include <Editor/Util/Toast.hpp>
#include <Engine/Scene/ScriptValidation.hpp>
#include <Engine/Core/Application.hpp>
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
    constexpr uint16_t OPEN_VFX         = 500;
    constexpr uint16_t TOGGLE_MAP       = 501;
    constexpr uint16_t OPEN_BUILD       = 502;
    constexpr uint16_t OPEN_IBL         = 503;
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
    terrainTools.push_back(command("Detail Tool", 512));
    terrainTools.push_back(command("Foliage Tool", 513));

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
        command("VFX Editor...", OPEN_VFX), command("Map Editing Mode", TOGGLE_MAP),
        submenu("Terrain & Map", std::move(terrainTools)),
        command("Build Settings...", OPEN_BUILD), command("IBL Baker...", OPEN_IBL)
    }));
    menus.push_back(submenu("AI", { command("AI Settings...", OPEN_AI_SETTINGS) }));

    m_window->SetNativeMenu(std::move(menus), [this](uint16_t id) {
        if (id >= PANEL_BASE && id < PANEL_BASE + m_panels.size()) {
            m_panels[id - PANEL_BASE]->visible = !m_panels[id - PANEL_BASE]->visible;
            return true;
        }

        switch (id) {
        case NEW_SCENE:       RequestNewScene(); break;
        case OPEN_SCENE:      RequestOpenSceneFromDialog(); break;
        case SAVE_SCENE:      SaveScene(); break;
        case SAVE_SCENE_AS:   SaveSceneAsDialog(); break;
        case CLOSE_PREFAB:    m_ctx.requestClosePrefabEdit = true; break;
        case SAVE_ALL_ASSETS: AssetDirtyRegistry::SaveAll(); break;
        case EXIT_EDITOR:     RequestExit(); break;
        case UNDO: if (m_ctx.undoStack && m_ctx.undoStack->CanUndo()) m_ctx.undoStack->Undo(); break;
        case REDO: if (m_ctx.undoStack && m_ctx.undoStack->CanRedo()) m_ctx.undoStack->Redo(); break;
        case PLAY:           StartPlayMode(); break;
        case STOP:           StopPlayMode(); break;
        case PAUSE:          if (m_playMode.IsPlaying() || m_playMode.IsPaused()) m_playMode.Pause(); break;
        case STEP:           m_playMode.RequestStep(); break;
        case RELOAD_SCRIPTS: m_ctx.requestScriptReload = true; break;
        case OPEN_ANALYSIS:  m_ctx.requestOpenAnalysis = true; break;
        case TOGGLE_GRID:    m_ctx.showGrid = !m_ctx.showGrid; break;
        case TOGGLE_LIGHTS:  m_ctx.showLightRange = !m_ctx.showLightRange; break;
        case TOGGLE_VFX:     m_ctx.showVFXGizmos = !m_ctx.showVFXGizmos; break;
        case TOGGLE_SKELETON:m_ctx.showSkeleton = !m_ctx.showSkeleton; break;
        case TOGGLE_STATS:   m_ctx.showStats = !m_ctx.showStats; break;
        case TOGGLE_HOTRELOAD: m_ctx.hotReloadEnabled = !m_ctx.hotReloadEnabled; break;
        case TOGGLE_COLLIDERS: m_ctx.projectSettings.render.showColliders = !m_ctx.projectSettings.render.showColliders; break;
        case TOGGLE_TERRAIN_COLLISION: m_ctx.projectSettings.render.showTerrainCollision = !m_ctx.projectSettings.render.showTerrainCollision; break;
        case TOGGLE_NAVMESH: m_ctx.projectSettings.render.showNavMesh = !m_ctx.projectSettings.render.showNavMesh; break;
        case TOGGLE_AI_SENSORS: m_ctx.projectSettings.render.showNavSensors = !m_ctx.projectSettings.render.showNavSensors; break;
        case TOGGLE_DECAL_BOUNDS: m_ctx.projectSettings.render.showDecalBounds = !m_ctx.projectSettings.render.showDecalBounds; break;
        case TOGGLE_SHADOW: m_ctx.projectSettings.render.shadowEnabled = !m_ctx.projectSettings.render.shadowEnabled; break;
        case VIEW_LIT:       m_ctx.projectSettings.render.viewMode = renderer::ViewMode::Lit; break;
        case VIEW_UNLIT:     m_ctx.projectSettings.render.viewMode = renderer::ViewMode::Unlit; break;
        case VIEW_WIRE_LIT:  m_ctx.projectSettings.render.viewMode = renderer::ViewMode::WireframeLit; break;
        case VIEW_WIRE_UNLIT:m_ctx.projectSettings.render.viewMode = renderer::ViewMode::WireframeUnlit; break;
        case 700:            m_ctx.editorUiScale = 1.0f; EditorTheme::SetUiScale(1.0f); break;
        case OPEN_VFX:       m_ctx.requestOpenVFXEditor = true; break;
        case TOGGLE_MAP:     m_ctx.requestMapEditingModeToggle = true; break;
        case OPEN_BUILD:     m_ctx.requestOpenBuildSettings = true; break;
        case OPEN_IBL:       if (m_iblBakePanel) m_iblBakePanel->visible = true; break;
        case OPEN_AI_SETTINGS: if (m_aiSettingsPanel) m_aiSettingsPanel->visible = true; break;
        case 510: m_ctx.showTerrainTool = !m_ctx.showTerrainTool; break;
        case 511: m_ctx.showWaterTool = !m_ctx.showWaterTool; break;
        case 512: m_ctx.showDetailTool = !m_ctx.showDetailTool; break;
        case 513: m_ctx.showFoliageTool = !m_ctx.showFoliageTool; break;
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
    if (ImGui::BeginMenu("File")) {
        // Prefab 編集モード中はシーン操作を伏せ、対象がアセットであることを明示する。
        // WHY: 項目名が "Save" のままだと、何が保存されるのかが読み取れない。
        const bool inPrefabEdit = ctx.InPrefabEditMode();
        if (ImGui::MenuItem("New Scene", nullptr, false, !inPrefabEdit))
            RequestNewScene();
        if (ImGui::MenuItem("Open...", "Ctrl+O", false, ctx.activeScene != nullptr && !inPrefabEdit))
            RequestOpenSceneFromDialog();
        if (ImGui::MenuItem(inPrefabEdit ? "Save Prefab" : "Save", "Ctrl+S",
                            false, ctx.activeScene != nullptr))
            SaveScene();   // 編集モード中は SaveScene が SavePrefabEdit へ読み替える
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false,
                            ctx.activeScene != nullptr && !inPrefabEdit))
            SaveSceneAsDialog();
        if (inPrefabEdit && ImGui::MenuItem("Close Prefab"))
            ctx.requestClosePrefabEdit = true;
        {
            const bool hasUnsaved = AssetDirtyRegistry::HasAny();
            if (ImGui::MenuItem("Save All Assets", nullptr, false, hasUnsaved))
                AssetDirtyRegistry::SaveAll();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) RequestExit();
        ImGui::EndMenu();
    }

    // --- Edit ------------------------------------------------------------
    if (ImGui::BeginMenu("Edit")) {
        const bool canUndo = ctx.undoStack && ctx.undoStack->CanUndo();
        const bool canRedo = ctx.undoStack && ctx.undoStack->CanRedo();
        const std::string undoLabel = canUndo
            ? "Undo " + ctx.undoStack->GetUndoDescription()
            : "Undo";
        const std::string redoLabel = canRedo
            ? "Redo " + ctx.undoStack->GetRedoDescription()
            : "Redo";
        if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z", false, canUndo)) ctx.undoStack->Undo();
        if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y", false, canRedo)) ctx.undoStack->Redo();
        ImGui::EndMenu();
    }

    // --- View ------------------------------------------------------------
    if (ImGui::BeginMenu("View")) {
        if (ImGui::BeginMenu("Panels")) {
            for (auto& panel : m_panels) {
                if (panel->ShowInViewMenu())
                    ImGui::MenuItem(panel->GetViewMenuName(), nullptr, &panel->visible);
            }
            ImGui::EndMenu();
        }

        // UI 全体スケール (フォント + 余白)。設定に永続化される。
        ImGui::Separator();
        ImGui::TextDisabled("UI Scale");
        float uiScale = m_ctx.editorUiScale;
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::SliderFloat("##ui_scale", &uiScale, 0.7f, 2.0f, "%.2fx")) {
            m_ctx.editorUiScale = uiScale;
            EditorTheme::SetUiScale(uiScale);
        }
        if (ImGui::MenuItem("Reset UI Scale", nullptr, false, m_ctx.editorUiScale != 1.0f)) {
            m_ctx.editorUiScale = 1.0f;
            EditorTheme::SetUiScale(1.0f);
        }
        ImGui::EndMenu();
    }

    // --- Debug -----------------------------------------------------------
    if (ImGui::BeginMenu("Debug")) {
        // WHY: Godot は表示パネル操作とデバッグ描画切替を別メニューに分けている。
        //      FBZZ でも View はレイアウト・パネル、Debug は実行/描画診断に寄せることで項目の意味を読み取りやすくする。
        if (ImGui::MenuItem("Analysis")) {
            ctx.requestOpenAnalysis = true;
        }
        ImGui::Separator();
        // --- Scene Overlays ---
        ImGui::MenuItem("Grid",        nullptr, &ctx.showGrid);
        ImGui::MenuItem("Light Range", nullptr, &ctx.showLightRange);
        ImGui::MenuItem("VFX Force Fields / Emitters", nullptr, &ctx.showVFXGizmos);
        ImGui::MenuItem("Skeleton",    nullptr, &ctx.showSkeleton);
        ImGui::MenuItem("Stats",       nullptr, &ctx.showStats);
        ImGui::Separator();
        // --- Physics / Rendering ---
        ImGui::MenuItem("Colliders",          nullptr, &ctx.projectSettings.render.showColliders);
        ImGui::MenuItem("Terrain Collision",  nullptr, &ctx.projectSettings.render.showTerrainCollision);
        ImGui::MenuItem("NavMesh",            nullptr, &ctx.projectSettings.render.showNavMesh);
        ImGui::MenuItem("AI Sensors",         nullptr, &ctx.projectSettings.render.showNavSensors);
        ImGui::MenuItem("Decal Bounds",       nullptr, &ctx.projectSettings.render.showDecalBounds);
        ImGui::Separator();
        // --- Tools ---
        ImGui::MenuItem("Hot Reload", nullptr, &ctx.hotReloadEnabled);
        ImGui::Separator();
        if (ImGui::BeginMenu("View Mode")) {
            auto& vm = ctx.projectSettings.render.viewMode;
            if (ImGui::MenuItem("Lit",            nullptr, vm == renderer::ViewMode::Lit))           vm = renderer::ViewMode::Lit;
            if (ImGui::MenuItem("Unlit",          nullptr, vm == renderer::ViewMode::Unlit))         vm = renderer::ViewMode::Unlit;
            if (ImGui::MenuItem("Wireframe Lit",  nullptr, vm == renderer::ViewMode::WireframeLit))  vm = renderer::ViewMode::WireframeLit;
            if (ImGui::MenuItem("Wireframe Unlit",nullptr, vm == renderer::ViewMode::WireframeUnlit))vm = renderer::ViewMode::WireframeUnlit;
            ImGui::EndMenu();
        }
        ImGui::Separator();
        // WHY Bloom / FXAA のトグルが無いか: ポストプロセスの所有者は
        //     Post Process Volume + Post Process Profile (.fzdata) へ一本化した。
        //     Shadow はプロジェクト全体の描画構成なのでここに残す。
        ImGui::MenuItem("Shadow", nullptr, &ctx.projectSettings.render.shadowEnabled);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("VFX Editor..."))
            ctx.requestOpenVFXEditor = true;
        ImGui::Separator();
        bool mapMode = ctx.mapEditingMode;
        if (ImGui::MenuItem("Map Editing Mode", nullptr, &mapMode))
            ctx.requestMapEditingModeToggle = true;
        ImGui::Separator();
        if (ImGui::BeginMenu("Terrain & Map")) {
            ImGui::MenuItem("Terrain Tool", nullptr, &ctx.showTerrainTool);
            ImGui::MenuItem("Water Tool",   nullptr, &ctx.showWaterTool);
            ImGui::MenuItem("Detail Tool",  nullptr, &ctx.showDetailTool);
            ImGui::MenuItem("Foliage Tool", nullptr, &ctx.showFoliageTool);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Build Settings...", "Ctrl+Shift+B")) {
            ctx.requestOpenBuildSettings = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("IBL Baker...")) {
            if (m_iblBakePanel) {
                m_iblBakePanel->visible = true;
                ImGui::SetNextWindowFocus();
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("AI")) {
        if (ImGui::MenuItem("AI Settings...")) {
            if (m_aiSettingsPanel) {
                m_aiSettingsPanel->visible = true;
                ImGui::SetNextWindowFocus();
            }
        }
        ImGui::Separator();
        const char* busStatus = ctx.aiCommandBusRunning
            ? "Editor Command Bus: Running"
            : "Editor Command Bus: Stopped";
        ImGui::MenuItem(busStatus, nullptr, false, false);
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
    const bool hasScene = ctx.activeScene != nullptr;
    const bool isEditor = pm && pm->IsInEditor();
    const bool isPlaying = pm && pm->IsPlaying();
    const bool isPaused = pm && pm->IsPaused();

    const float toolbarButtonY = ImGui::GetCursorPosY();
    const bool canToggleMapMode = isEditor && hasScene;
    if (ctx.mapEditingMode)
        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(ThemeColor::Secondary));
    if (!canToggleMapMode)
        ImGui::BeginDisabled();
    if (ImGui::Button(ctx.mapEditingMode ? "EXIT MAP" : "MAP MODE", { 92.0f, 24.0f }))
        ctx.requestMapEditingModeToggle = true;
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
            ImGui::SetTooltip("Enter Map Editing Mode\nTerrain / Water / Detail / Foliage tools in a focused layout");
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
        pm && hasScene && isEditor && !scriptReloadBusy,
        isPlaying,
        playColor,
        BUTTON_SIZE)) {
        StartPlayMode();
    }

    ImGui::SameLine();
    if (PlayToolbarButton(
        "##PlayModeStop",
        "Stop",
        PlayToolbarIcon::Stop,
        pm && hasScene && !isEditor,
        isEditor,
        stopColor,
        BUTTON_SIZE)) {
        StopPlayMode();
    }

    ImGui::SameLine();
    if (PlayToolbarButton(
        "##PlayModePause",
        isPaused ? "Resume" : "Pause",
        PlayToolbarIcon::Pause,
        pm && !isEditor,
        isPaused,
        pauseColor,
        BUTTON_SIZE))
        pm->Pause();

    ImGui::SameLine();
    if (PlayToolbarButton(
        "##PlayModeStep",
        "Step",
        PlayToolbarIcon::Step,
        pm && isPaused,
        false,
        pauseColor,
        BUTTON_SIZE))
        pm->RequestStep();

    ImGui::SameLine();
    {
        const ImVec4 reloadColor { 0.25f, 0.55f, 0.90f, 1.0f };
        const char* reloadTooltip = scriptReloadBusy ? "Scripts are compiling..." : "Reload Scripts";
        if (PlayToolbarButton(
            "##ScriptReload",
            reloadTooltip,
            PlayToolbarIcon::Reload,
            !scriptReloadBusy,
            scriptReloadBusy,
            reloadColor,
            BUTTON_SIZE))
            ctx.requestScriptReload = true;
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
    //
    // WHY ここか: 付け忘れは「動かないけどエラーも出ない」という形でしか現れないため、
    //     気付くのは大抵 Play したあと。Play を押した瞬間に Console へ全件出しておけば、
    //     ゲームの挙動を目で追う前に原因が名指しで並んでいる状態から始められる。
    // WHY Play を止めないか: 不足があっても他の部分は動くし、そもそも作りかけの
    //     シーンを走らせて確かめるのが Play の役目。止めると「試せない」ほうの
    //     コストが上回る。Unity の Console と同じく、報告はするが進行は妨げない。
    if (const auto issues = scene::ValidateSceneScriptRequirements(*m_ctx.activeScene);
        !issues.empty()) {
        for (const auto& issue : issues)
            FBZZ_LOG_ERROR("Script requirement: %s",
                           scene::FormatScriptRequirementIssue(issue).c_str());
        Toast::Error(std::to_string(issues.size()) +
                     " missing script component(s) — see Console");
    }

    m_undoStack.Clear();
    RemoveEditorHiding();  // Play 前に editor-only 非表示を一時解除（スナップショットに active 状態で含める）
    // navMesh は TOML に保存されないため、Play 開始前にキャッシュしておく。
    // Stop 後の scene 復元で needsBake=true が立っても再ベイクせずに済む。
    m_navMeshPlayCache.clear();
    for (scene::EntityID eid : m_ctx.activeScene->GetEntities<scene::NavMeshSurfaceComponent>()) {
        auto* surf = m_ctx.activeScene->GetComponent<scene::NavMeshSurfaceComponent>(eid);
        auto* go   = m_ctx.activeScene->GetGameObject(eid);
        if (surf && go && surf->navMesh.IsValid())
            m_navMeshPlayCache[go->instanceId] = surf->navMesh;
    }
    // WHY: ScriptProxy は ScriptRuntime 経由でサブシステムを参照する。
    //      エディタは共通ProjectRuntimeのSceneManagerをUpdateするため、Play開始時に
    //      ScriptRuntime をオーバーライドして正しい参照先を指す。
    m_runtime.ActivateScriptRuntime(
        core::Application::Get().GetRenderer(),
        static_cast<uint32_t>(m_ctx.gameViewportWidth),
        static_cast<uint32_t>(m_ctx.gameViewportHeight)
    );
    m_playMode.Play(*m_ctx.activeScene);
    if (m_playMode.IsPlaying() && m_ctx.playFocusMode != EditorContext::PlayFocusMode::Unfocused)
        m_ctx.requestGameViewportFocus = true;
}

void EditorApp::StopPlayMode()
{
    if (!m_ctx.activeScene || m_playMode.IsInEditor())
        return;
    scene::ScriptRuntime::Override(nullptr);
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

// =============================================================================
// ホットキー登録 — エディター内の全ショートカットの単一の定義場所
//
// WHY: ここに無いキーはリバインドできず、F1 の一覧にも出ない。逆に言えば、
//      ここに書けば入力処理・一覧・リバインド UI・設定への永続化が全部ついてくる。
//      パネル側で IsKeyPressed を直接叩くと、その 4 つが揃わないものが増える。
// =============================================================================
void EditorApp::RegisterDefaultHotkeys()
{
    using Cat   = HotkeyCategory;
    using Scope = HotkeyScope;

    // 「シーンを編集できる状態か」— Play 中とプレファブ編集中は対象が違う。
    const auto canEditScene = [this]() {
        return m_ctx.activeScene != nullptr && m_playMode.IsInEditor();
    };
    const auto hasSelection = [this]() { return !m_ctx.selectedEntities.empty(); };
    const auto canEditSelection = [this, canEditScene, hasSelection]() {
        return canEditScene() && hasSelection();
    };

    // Register の引数が増えたので、指定を読みやすくする小さな組み立てヘルパー。
    auto add = [this](const char* name, int key, bool ctrl, bool shift, bool alt,
                      Scope scope, Cat category,
                      std::function<void()> callback,
                      std::function<bool()> enabled = {}) {
        Hotkey hk;
        hk.name     = name;
        hk.imguiKey = key;
        hk.ctrl     = ctrl;
        hk.shift    = shift;
        hk.alt      = alt;
        hk.scope    = scope;
        hk.category = category;
        hk.callback = std::move(callback);
        hk.enabled  = std::move(enabled);
        m_hotkeys.Register(std::move(hk));
    };

    // ── File ────────────────────────────────────────────────────────────────
    add("New Scene",     ImGuiKey_N, true, false, false, Scope::Global, Cat::File,
        [this]() { RequestNewScene(); });
    add("Open Scene",    ImGuiKey_O, true, false, false, Scope::Global, Cat::File,
        [this]() { RequestOpenSceneFromDialog(); });
    add("Save",          ImGuiKey_S, true, false, false, Scope::Global, Cat::File,
        [this]() { SaveScene(); });   // Prefab 編集中はプレファブ保存に読み替わる
    add("Save Scene As", ImGuiKey_S, true, true,  false, Scope::Global, Cat::File,
        [this]() { SaveSceneAsDialog(); });

    // ── Edit ────────────────────────────────────────────────────────────────
    add("Undo", ImGuiKey_Z, true, false, false, Scope::Global, Cat::Edit,
        [this]() { m_undoStack.Undo(); },
        [this]() { return m_undoStack.CanUndo(); });
    add("Redo", ImGuiKey_Y, true, false, false, Scope::Global, Cat::Edit,
        [this]() { m_undoStack.Redo(); },
        [this]() { return m_undoStack.CanRedo(); });
    // WHY: Ctrl+Shift+Z は Unity / Photoshop 系の Redo。Ctrl+Y と併存させ、
    //      どちらの操作習慣のユーザーでも迷わないようにする。
    add("Redo (Alt)", ImGuiKey_Z, true, true, false, Scope::Global, Cat::Edit,
        [this]() { m_undoStack.Redo(); },
        [this]() { return m_undoStack.CanRedo(); });

    // Scene View と Hierarchy のどちらにフォーカスがあっても効く編集操作。
    // WHY: Unity では Scene View で選んだまま Delete / Ctrl+D が効く。
    //      Hierarchy へフォーカスを移さないと消せないのは動線として遠回り。
    constexpr Scope kEditScopes = Scope::SceneViewport | Scope::Hierarchy;

    add("Delete Selected", ImGuiKey_Delete, false, false, false, kEditScopes, Cat::Edit,
        [this]() { DeleteSelectedWithUndo(m_ctx); }, canEditSelection);
    add("Duplicate", ImGuiKey_D, true, false, false, kEditScopes, Cat::Edit,
        [this]() { DuplicateSelectedWithUndo(m_ctx); }, canEditSelection);
    add("Copy", ImGuiKey_C, true, false, false, kEditScopes, Cat::Edit,
        [this]() { CopySelectedToClipboard(m_ctx); }, canEditSelection);
    add("Paste", ImGuiKey_V, true, false, false, kEditScopes, Cat::Edit,
        [this]() { PasteClipboardWithUndo(m_ctx); },
        [this, canEditScene]() { return canEditScene() && HasGameObjectClipboard(); });
    add("Paste As Child", ImGuiKey_V, true, true, false, kEditScopes, Cat::Edit,
        [this]() {
            PasteClipboardWithUndo(m_ctx, m_ctx.selectedEntities.size() == 1
                                              ? m_ctx.selectedEntities[0]
                                              : scene::EntityID{});
        },
        [this, canEditScene]() { return canEditScene() && HasGameObjectClipboard(); });
    add("Rename", ImGuiKey_F2, false, false, false, Scope::Hierarchy, Cat::Edit,
        [this]() { m_ctx.requestRenameSelected = true; },
        [this, canEditScene]() {
            return canEditScene() && m_ctx.selectedEntities.size() == 1;
        });

    // ── Selection ───────────────────────────────────────────────────────────
    add("Select All", ImGuiKey_A, true, false, false, kEditScopes, Cat::Selection,
        [this]() {
            m_ctx.selectedEntities.clear();
            for (auto& go : m_ctx.activeScene->GameObjects())
                if (!m_ctx.IsLocked(go.GetID()))
                    m_ctx.selectedEntities.push_back(go.GetID());
        },
        canEditScene);
    add("Clear Selection", ImGuiKey_Escape, false, false, false, kEditScopes, Cat::Selection,
        [this]() { m_ctx.selectedEntities.clear(); }, hasSelection);
    add("Selection Back", ImGuiKey_LeftArrow, false, false, true, Scope::Global, Cat::Selection,
        [this]() { NavigateSelectionHistory(-1); });
    add("Selection Forward", ImGuiKey_RightArrow, false, false, true, Scope::Global, Cat::Selection,
        [this]() { NavigateSelectionHistory(1); });

    // ── Viewport ────────────────────────────────────────────────────────────
    add("Frame Selected", ImGuiKey_F, false, false, false, Scope::SceneViewport, Cat::Viewport,
        [this]() {
            math::Vector3 center{};
            float radius = 0.0f;
            if (!ComputeSelectionBounds(m_ctx, center, radius)) return;
            m_ctx.focusTargetPosition    = center;
            m_ctx.focusTargetRadius      = radius;
            m_ctx.requestFocusOnSelected = true;
        },
        hasSelection);

    // ── Gizmo ───────────────────────────────────────────────────────────────
    // WHY: 右ドラッグ中の W/A/S/D はカメラのフライ移動。ギズモ切替と衝突するので、
    //      押下中は無効にする (Unity と同じ調停)。
    const auto gizmoEnabled = [this]() {
        return !ImGui::IsMouseDown(ImGuiMouseButton_Right) && m_playMode.IsInEditor();
    };
    add("Gizmo: Move", ImGuiKey_W, false, false, false, Scope::SceneViewport, Cat::Gizmo,
        [this]() { m_ctx.gizmoMode = EditorContext::GizmoMode::Translate; }, gizmoEnabled);
    add("Gizmo: Rotate", ImGuiKey_E, false, false, false, Scope::SceneViewport, Cat::Gizmo,
        [this]() { m_ctx.gizmoMode = EditorContext::GizmoMode::Rotate; }, gizmoEnabled);
    add("Gizmo: Scale", ImGuiKey_R, false, false, false, Scope::SceneViewport, Cat::Gizmo,
        [this]() { m_ctx.gizmoMode = EditorContext::GizmoMode::Scale; }, gizmoEnabled);
    add("Gizmo: World / Local", ImGuiKey_Q, false, false, false, Scope::SceneViewport, Cat::Gizmo,
        [this]() {
            m_ctx.gizmoSpace = (m_ctx.gizmoSpace == EditorContext::GizmoSpace::World)
                ? EditorContext::GizmoSpace::Local
                : EditorContext::GizmoSpace::World;
        }, gizmoEnabled);
    add("Gizmo: Pivot / Center", ImGuiKey_Z, false, false, false, Scope::SceneViewport, Cat::Gizmo,
        [this]() {
            m_ctx.gizmoPivot = (m_ctx.gizmoPivot == EditorContext::GizmoPivot::Pivot)
                ? EditorContext::GizmoPivot::Center
                : EditorContext::GizmoPivot::Pivot;
        }, gizmoEnabled);
    add("Toggle Grid Snap", ImGuiKey_X, false, false, false, Scope::SceneViewport, Cat::Gizmo,
        [this]() { m_ctx.snapEnabled = !m_ctx.snapEnabled; }, gizmoEnabled);

    // ── Play ────────────────────────────────────────────────────────────────
    add("Play / Stop", ImGuiKey_P, true, false, false, Scope::Global, Cat::Play,
        [this]() { TogglePlayMode(); });
    add("Pause", ImGuiKey_P, true, true, false, Scope::Global, Cat::Play,
        [this]() { if (!m_playMode.IsInEditor()) m_playMode.Pause(); },
        [this]() { return !m_playMode.IsInEditor(); });

    // ── Panels ──────────────────────────────────────────────────────────────
    add("Command Palette", ImGuiKey_K, true, false, false, Scope::Global, Cat::Panels,
        [this]() { m_commandPaletteOpen = true; });
    add("Shortcut List", ImGuiKey_F1, false, false, false, Scope::Global, Cat::Panels,
        [this]() { m_showShortcutsOverlay = !m_showShortcutsOverlay; });
    // 独立VFXEditorはEditorのDock/Focus状態に依存せず、専用プロセスとして起動する。
    add("Open VFX Editor", ImGuiKey_V, true, false, true, Scope::Global, Cat::Panels,
        [this]() { m_ctx.requestOpenVFXEditor = true; });

    // ── 説明専用エントリ ─────────────────────────────────────────────────────
    // WHY: マウス操作や数字キー列はキー 1 つに割り当てられないが、
    //      「どう操作するか」の一覧としては同じくらい知りたい情報。別表に切り出すと
    //      そこがまた手書きの二重管理になるので、同じ器に入れて一覧を 1 本に保つ。
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

    // NOTE: 保存済みリバインドの適用は呼び出し側 (EditorApp::Init) が
    //       この直後に行う。既定値を全部積んだ後でないと Rebind が対象を引けないため。

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
