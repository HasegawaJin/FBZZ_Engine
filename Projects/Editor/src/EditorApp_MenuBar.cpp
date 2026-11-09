/// @file    EditorApp_MenuBar.cpp
/// @brief   メインメニューバーの構築とホットキー登録。
/// @author  Hasegawa Jin
/// @date    2026-05-31

/// @note UI 記述に特化した独立ファイル。ライフサイクル管理やシーン I/O と関心を分離する。
#include <Editor/EditorApp.hpp>
#include <Editor/Util/SpatialDebugOverlay.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Panels/AssetMaintenancePanel.hpp>
#include <Editor/Panels/IblBakePanel.hpp>
#include <Editor/Panels/VolumeFlipbookBakePanel.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/ManagedScriptEditor.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/CreateObjectMenu.hpp>
#include <Editor/Util/EditorMenuCatalog.hpp>
#include <Editor/Util/HotkeyManager.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/Localization.hpp>
#include <Editor/Util/ObjectPresets.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
#include <Editor/Util/Toast.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/RenderPipelineAsset.hpp>
#include <Engine/Scene/ScriptValidation.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Cursor.hpp>
#include <Engine/Core/DeveloperMode.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Window.hpp>
#include <Engine/Audio/AudioManager.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Scene/Systems/DirectionalShadowBakeSystem.hpp>
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
    /// @note        active 時は操作種別の色を背景へ乗せ、無効時は Disabled スタイルで入力も止める。
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
    struct NativeCommand { std::string operatorId; OpArgs args; };
    std::vector<NativeCommand> commands;
    const auto command = [this, &commands](const char* operatorId, const char* label = nullptr, OpArgs args = {}) {
        const EditorOperator* op = m_operators.Find(operatorId);
        std::string title = LOCT(label ? label : (op ? op->label.c_str() : operatorId));
        if (const Hotkey* hotkey = m_hotkeys.FindByOperator(operatorId))
            title += "\t" + HotkeyManager::FormatBinding(*hotkey);
        commands.push_back({operatorId, std::move(args)});
        return NativeMenuItem{util::StringUtils::ToWide(title), static_cast<uint16_t>(commands.size()), false, {}};
    };
    const auto separator = []() { return NativeMenuItem{{}, 0, true, {}}; };
    const auto submenu = [](const char* label, MenuList children) {
        return NativeMenuItem{util::StringUtils::ToWide(LOCT(label)), 0, false, std::move(children)};
    };
    const auto actions = [&command](std::span<const EditorMenuAction> entries) {
        MenuList items;
        for (const auto& entry : entries) {
            OpArgs args;
            if (entry.mode) args.Set("mode", std::string(entry.mode));
            items.push_back(command(entry.operatorId, entry.label, std::move(args)));
        }
        return items;
    };

    MenuList panels;
    for (const auto& group : BuildPanelMenu(m_panels)) {
        MenuList entries;
        for (const auto& entry : group.entries) {
            OpArgs args;
            args.Set("panel", std::string(entry.panel->GetWindowName()));
            entries.push_back(command("panel.set_visible", entry.panel->GetViewMenuName(), std::move(args)));
        }
        if (group.category.empty()) panels = std::move(entries);
        else panels.push_back(submenu(group.category.c_str(), std::move(entries)));
    }
    panels.push_back(separator());
    panels.push_back(command("panel.command_palette"));
    panels.push_back(submenu("Scene Camera", {
        command("view.toggle_projection"), separator(),
        command("view.axis_front"), command("view.axis_back"), command("view.axis_left"),
        command("view.axis_right"), command("view.axis_top"), command("view.axis_bottom"),
        separator(), command("view.frame_selected")
    }));
    MenuList overlays;
    for (const auto& group : SCENE_OVERLAY_GROUPS) {
        MenuList entries = actions(group.actions);
        if (group.category[0] == '\0') overlays = std::move(entries);
        else {
            if (std::string_view(group.category) == "Navigation")
                entries.push_back(submenu("NavMesh Draw Mode", actions(NAVMESH_DRAW_MODES)));
            overlays.push_back(submenu(group.category, std::move(entries)));
        }
    }
    panels.push_back(submenu("Scene Overlays", std::move(overlays)));
    MenuList modes = actions(SCENE_VIEW_MODES);
    if (core::DeveloperMode::IsEnabled()) {
        MenuList developerModes = actions(DEVELOPER_VIEW_MODES);
        modes.insert(modes.end(), developerModes.begin(), developerModes.end());
    }
    panels.push_back(submenu("View Mode", std::move(modes)));
    panels.push_back(submenu("UI Scale", {command("view.reset_ui_scale")}));

    MenuList create;
    for (const ObjectPreset& preset : ObjectPresetCatalog()) {
        OpArgs args;
        args.Set("preset", std::string(preset.id));
        args.Set("placeInView", true);
        NativeMenuItem item = command("node.create_preset", std::string(preset.label).c_str(), std::move(args));
        if (preset.category.empty()) create.push_back(std::move(item));
        else {
            const std::wstring category = util::StringUtils::ToWide(LOCT(std::string(preset.category).c_str()));
            auto group = std::find_if(create.begin(), create.end(), [&category](const NativeMenuItem& entry) {
                return entry.commandId == 0 && entry.label == category;
            });
            if (group == create.end()) {
                create.push_back(submenu(std::string(preset.category).c_str(), {}));
                group = create.end() - 1;
            }
            group->children.push_back(std::move(item));
        }
    }

    MenuList tools {command("tools.map_editing_mode"), command("tools.terrain"), separator(),
                    command("tools.build_settings")};
    const auto panelTool = [&command](IPanel* panel, const char* label) {
        OpArgs args;
        args.Set("panel", std::string(panel->GetWindowName()));
        return command("panel.focus", label, std::move(args));
    };
    if (m_iblBakePanel) tools.push_back(panelTool(m_iblBakePanel, "IBL Baker..."));
    if (m_volumeFlipbookBakePanel) tools.push_back(panelTool(m_volumeFlipbookBakePanel, "Volume Flipbook Baker..."));
    if (m_assetMaintenancePanel) tools.push_back(panelTool(m_assetMaintenancePanel, "Asset Maintenance..."));
    MenuList menus;
    menus.push_back(submenu("File", {command("scene.new"), command("scene.open"), command("scene.save"),
        command("scene.save_as"), command("prefab.close"), command("asset.save_all"), separator(), command("app.exit")}));
    menus.push_back(submenu("Edit", {command("edit.undo"), command("edit.redo"), separator(),
        command("edit.duplicate"), command("edit.delete_selected"), separator(), command("edit.copy"),
        command("edit.paste"), command("edit.paste_as_child")}));
    menus.push_back(submenu("GameObject", std::move(create)));
    menus.push_back(submenu("Play", {command("play.start"), command("play.stop"), command("play.pause"), command("play.step")}));
    menus.push_back(submenu("View", std::move(panels)));
    menus.push_back(submenu("Debug", {command("render.scene_view_occlusion_culling", "Scene View Occlusion Culling"),
        command("render.shadow_enabled", "Shadow"), separator(), command("debug.hot_reload", "Hot Reload"),
        command("debug.hot_reload_sound", "Hot Reload Sound"), command("ai.command_bus", "Editor Command Bus"),
        command("tools.pix_open"), separator(),
        command("developer.toggle_mode", "Developer Mode")}));
    menus.push_back(submenu("Tools", std::move(tools)));
    /// @note ネイティブ ID はこのメニューの存続期間だけ有効。操作 ID と保存済みホットキーには影響しない。
    m_window->SetNativeMenu(std::move(menus), [this, commands = std::move(commands)](uint16_t id) {
        if (id == 0 || id > commands.size()) return false;
        const NativeCommand& selected = commands[id - 1];
        InvokeOperator(selected.operatorId, selected.args);
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
        /// @note        表示していない状況では AI からも通らない。
        if (inPrefabEdit) MenuItemOp("prefab.close");
        MenuItemOp("asset.save_all");
        ImGui::Separator();
        MenuItemOp("app.exit");
        ImGui::EndMenu();
    }

    /// @name Edit
    if (ImGui::BeginMenu(LOC("Edit"))) {
        /// @note 直前の操作名を添えて「何が戻るのか」を読めるようにする。
        /// @note        ラベルだけが動的で、実行可否と実体は operator 側にある。
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
        DrawCreateObjectMenu(ctx, pendingCreate);
        ImGui::EndMenu();
        InvokePendingObjectCreate(ctx, pendingCreate);
    }

    /// @name View
    if (ImGui::BeginMenu(LOC("View"))) {
        /// @note 分類はネイティブメニューとパレットも共有し、設定に使うウィンドウ ID を保持する。
        for (const auto& group : BuildPanelMenu(m_panels)) {
            const bool nested = !group.category.empty();
            if (nested && !ImGui::BeginMenu(LOC(group.category.c_str()))) continue;
            for (const auto& entry : group.entries) {
                OpArgs args;
                args.Set("panel", std::string(entry.panel->GetWindowName()));
                MenuItemOpArgs("panel.set_visible", args, entry.panel->GetViewMenuName());
            }
            if (nested) ImGui::EndMenu();
        }
        ImGui::Separator();
        MenuItemOp("panel.command_palette");

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

        const auto drawActions = [this](std::span<const EditorMenuAction> entries) {
            for (const auto& entry : entries) {
                OpArgs args;
                if (entry.mode) args.Set("mode", std::string(entry.mode));
                MenuItemOpArgs(entry.operatorId, args, entry.label);
            }
        };
        if (ImGui::BeginMenu(LOC("Scene Overlays"))) {
            for (const auto& group : SCENE_OVERLAY_GROUPS) {
                const bool nested = group.category[0] != '\0';
                if (nested && !ImGui::BeginMenu(LOC(group.category))) continue;
                drawActions(group.actions);
                if (std::string_view(group.category) == "Navigation"
                    && ImGui::BeginMenu(LOC("NavMesh Draw Mode"))) {
                    drawActions(NAVMESH_DRAW_MODES);
                    ImGui::EndMenu();
                }
                if (nested) ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(LOC("View Mode"))) {
            drawActions(SCENE_VIEW_MODES);
            if (core::DeveloperMode::IsEnabled()) drawActions(DEVELOPER_VIEW_MODES);
            ImGui::EndMenu();
        }

        /// @note UI 全体スケール (フォント + 余白)。設定に永続化される。
        /// @note        スライダー自体は連続値のドラッグなので operator には乗らないが、
        /// @note        適用は view.set_ui_scale を通す (範囲の宣言と適用処理を 1 箇所に保つ)。
        if (ImGui::BeginMenu(LOC("UI Scale"))) {
            /// @note 範囲は operator の params 宣言から引く。スライダー側に直書きすると、
            /// @note        人は 0.5x にできるのに AI からは BAD_ARG で弾かれる (同じ操作の限界が
            /// @note        面ごとに違う) という、この設計が消したいずれが範囲という形で再発する。
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
        ImGui::EndMenu();
    }

    /// @name Debug
    if (ImGui::BeginMenu(LOC("Debug"))) {
        /// @name Culling
        /// @note Scene View はデバッグカメラで描くため CameraComponent の設定が届かない。
        /// @note        「消えた原因がカリングか」を切り分ける唯一の口なので Debug 側へ出す。
        MenuItemOp("render.scene_view_occlusion_culling", "Scene View Occlusion Culling");
        ImGui::Separator();
        /// @name Tools
        MenuItemOp("debug.hot_reload", "Hot Reload");
        MenuItemOp("debug.hot_reload_sound", "Hot Reload Sound");
        MenuItemOp("ai.command_bus", "Editor Command Bus");
        ImGui::Separator();
        /// @note Bloom / FXAA は Post Process Volume + Profile (.fzdata) へ一本化済み。
        /// @note        Shadow はプロジェクト全体の描画構成なのでここに残す。
        MenuItemOp("render.shadow_enabled", "Shadow");
        ImGui::Separator();
        MenuItemOp("tools.pix_open");
        const auto pixStatus = InvokeOperator("tools.pix_status");
        const auto* pixReady = pixStatus.data.Find("captureReady");
        ImGui::TextDisabled("%s", LOC(pixReady && pixReady->AsBool()
            ? "PIX: GPU capture ready" : "PIX: next launch with --pix-capture"));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pixStatus.message.c_str());
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
        MenuItemOp("tools.terrain");
        MenuItemOp("tools.build_settings");
        ImGui::Separator();
        /// @note パネルごとに operator を生やすと m_panels という単一の出所が二重管理へ戻る。
        /// @note        名前を引数で渡す 1 つの操作で足りる。
        if (m_iblBakePanel && ImGui::MenuItem(LOC("IBL Baker...")))
            InvokePanelFocus(m_iblBakePanel);
        if (m_volumeFlipbookBakePanel && ImGui::MenuItem(LOC("Volume Flipbook Baker...")))
            InvokePanelFocus(m_volumeFlipbookBakePanel);
        if (m_assetMaintenancePanel && ImGui::MenuItem(LOC("Asset Maintenance...")))
            InvokePanelFocus(m_assetMaintenancePanel);
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
    const bool standaloneActive = m_standalonePlay.IsActive();
    const bool isEditor = pm && pm->IsInEditor() && !standaloneActive;
    const bool isPlaying = (pm && pm->IsPlaying()) || standaloneActive;
    const bool isPaused = pm && pm->IsPaused();

    const float groupWidth = BUTTON_SIZE.x * 4.0f + BUTTON_SPACING * 3.0f;
    const float availableWidth = ImGui::GetWindowWidth();
    const float centerOffset = (std::max)(8.0f, (availableWidth - groupWidth) * 0.5f);
    ImGui::SetCursorPosX(centerOffset);
    const bool managedBuildBusy = ctx.managedBuildBusy;
    const char* playTooltip = "Play";
    if (managedBuildBusy) playTooltip = "Play is available after C# or shader builds finish";
    else if (m_pendingShadowBakePlayScene) playTooltip = "Preparing baked shadows before Play";
    else if (isPaused) playTooltip = "Resume from Play Mode";

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
        m_pendingShadowBakePlayScene ? "Cancel pending Play" : "Stop",
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


    /// @name 右端: ホットリロードステータス / PLAYING ラベル
    {
        /// @note ホットリロードステータステキストを決定する
        const char* reloadText  = nullptr;
        ImVec4      reloadColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        switch (ctx.hotReloadState) {
        case EditorContext::HotReloadState::Compiling:
            reloadText  = "Compiling shaders...";
            reloadColor = { 1.0f, 0.85f, 0.2f,  1.0f };
            break;
        case EditorContext::HotReloadState::Reloading:
            reloadText  = "Reloading shaders...";
            reloadColor = { 0.5f, 0.8f,  1.0f,  1.0f };
            break;
        case EditorContext::HotReloadState::Done:
            reloadText  = "Shaders reloaded";
            reloadColor = { 0.35f, 1.0f, 0.45f, 1.0f };
            break;
        case EditorContext::HotReloadState::Failed:
            /// @note コンパイル失敗とロード失敗を区別する。以前はどちらも «Compile Error» だった。
            reloadText  = ctx.hotReloadMessage.find("error") != std::string::npos
                ? "Shader errors"
                : "Shader reload failed";
            reloadColor = { 1.0f, 0.35f, 0.35f, 1.0f };
            break;
        default: break;
        }

        /// @note PLAYING / PAUSED ラベル
        const char*  playLabel    = standaloneActive
            ? (m_standalonePlay.IsStopping() ? "STOPPING STANDALONE" : "STANDALONE")
            : (!isEditor ? (isPlaying ? "PLAYING" : "PAUSED") : nullptr);
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
            /// @note        Compiling / PLAYING 表示が Play ボタンを圧迫するより、操作ボタンの完全表示を優先する。
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
                if (playLabel) {
                    ImGui::TextColored(playLabelCol, "%s", playLabel);
                    if (standaloneActive && ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\nClick to open the session log", m_standalonePlay.Diagnostic().c_str());
                    if (standaloneActive && ImGui::IsItemClicked()) OpenStandalonePlayLog();
                }
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

/// @note  ビルド失敗通知バー

/// @note ビルド失敗が残っている間はツールバー直下に赤帯を出し続け、[Show] / [Dismiss] で操作する。
/// @note        成功ビルドで自動的に消える。
void EditorApp::DrawBuildNotificationBar(EditorContext& ctx)
{
    if (!ctx.buildConsole || !ctx.buildConsole->HasActiveFailure()) return;

    const BuildRecord* fail = ctx.buildConsole->LatestFailure();
    if (!fail) return;

    const float barH = ImGui::GetFrameHeight() + 4.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Color(ThemeColor::SurfaceRaised));
    ImGui::BeginChild("##BuildFailBar", ImVec2(0.0f, barH), false, ImGuiWindowFlags_NoScrollbar);

    ImGui::AlignTextToFramePadding();
    const char* kind = "HLSL";
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

void EditorApp::DrawGuidConflictBar(EditorContext&)
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

/// @note  ホットキー登録

void EditorApp::StartPlayMode(bool shadowBakePrepared, bool allowPhysicalInput)
{
    if (RejectWhileSceneSaving()) return;
    if (m_packageBuild.IsRunning() || m_pendingShadowBakePlayScene) return;
    /// @note Play ツールバーボタンと Ctrl+P ホットキーの共通経路。
    /// @note        ガード条件をここに集約し、どの入力経路でも同じ前提チェックを通す。
    const bool managedBuildBusy = m_ctx.managedBuildBusy;
    if (!m_ctx.activeScene || !m_playMode.IsInEditor() || m_standalonePlay.IsActive() || managedBuildBusy)
        return;
    /// @note Prefab 編集面には「シーン」が無い (カメラもライトも無い、プレファブ単体)。
    /// @note        そのまま Play すると空舞台で動き出し、Stop 時の復元も Prefab 内容に対して行われるため、
    /// @note        編集面から出るまで開始させない。
    if (m_ctx.InPrefabEditMode()) {
        FBZZ_LOG_WARN("Play: close the prefab edit mode first");
        return;
    }

    m_playAllowsPhysicalInput = allowPhysicalInput;
    if (m_ctx.playExecutionMode == PlayExecutionMode::STANDALONE) {
        StartStandalonePlayMode(allowPhysicalInput);
        return;
    }

    if (!shadowBakePrepared) {
        const auto cacheDir = util::FileSystem::PathFromUtf8(m_ctx.projectRoot) / "Library" / "DirectionalShadowBake";
        const auto status = scene::RequestDirectionalShadowBake(*m_scene, cacheDir);
        if (status == scene::DirectionalShadowBakeStatus::PENDING) {
            m_pendingShadowBakePlayScene = m_scene->GetRenderSceneGeneration();
            m_pendingShadowBakeAllowPhysicalInput = allowPhysicalInput;
            Toast::Info("Preparing baked shadows before Play");
            return;
        }
        if (status == scene::DirectionalShadowBakeStatus::UNAVAILABLE)
            Toast::Warning("Shadow bake unavailable. Play will use realtime shadows.");
    }

    /// @note 一覧と同じ検証を行う。Awake による設定を許すため、開始可否はランタイムで再判定する。
    OpArgs requirementArgs;
    requirementArgs.Set("showPanel", false);
    editor::InvokeOperator(m_ctx, "script.requirements.validate", requirementArgs);
    const auto& requirementReview = m_ctx.scriptRequirementReview;
    if (!requirementReview.rows.empty()) {
        for (const auto& row : requirementReview.rows)
            FBZZ_LOG_ERROR("Script requirement: %s",
                           scene::FormatScriptRequirementIssue(row.issue).c_str());
        Toast::Error(std::to_string(requirementReview.rows.size()) +
                     " required setting issue(s) — Script Requirements / Console");
    }

    std::vector<std::string> selected;
    for (const auto id : m_ctx.selectedEntities) if (auto* object = m_scene->GetGameObject(id)) selected.push_back(object->instanceId);
    /// @note Apply before clone callbacks so an automated start cannot briefly capture the host cursor.
    m_playPreviousPhysicalInputAllowed = core::Cursor::IsPhysicalInputAllowed();
    m_playInputPolicyApplied = true;
    core::Cursor::SetPhysicalInputAllowed(allowPhysicalInput);
    m_ctx.requestGameViewportFocus = false;
    m_ctx.requestGameCursorCapture = false;
    m_playMode.Play(*m_scene, m_ctx.currentScenePath, m_resources);
    if (!m_playMode.IsPlaying()) {
        core::Cursor::SetPhysicalInputAllowed(m_playPreviousPhysicalInputAllowed);
        m_playInputPolicyApplied = false;
        Toast::Error(m_playMode.Diagnostic());
        return;
    }
    if (m_managedScripts) m_managedScripts->BeginPlaySession();
    m_ctx.activeScene = m_playMode.PlayScene(); m_ctx.runtime = m_playMode.PlayRuntime();
    m_ctx.undoStack = m_playMode.TemporaryUndo();
    ClearEntitySelection(m_ctx);
    for (const auto& guid : selected) if (auto* object = m_ctx.activeScene->FindByGuid(guid)) m_ctx.selectedEntities.push_back(object->GetID());
    /// @note Restore editor-only visibility on the clone, leaving original authoring objects untouched.
    RemoveEditorHiding();
    auto& runtime = ActiveRuntime(); runtime.ApplySettings(m_ctx.projectSettings);
    runtime.GetSceneManager().SetAudioManager(core::Application::Get().GetAudioManager());
    runtime.RegisterScenes(util::FileSystem::PathFromUtf8(m_ctx.projectRoot), *m_resources);
    core::Cursor::ClearRequests(); m_ctx.projectSettings.cursor.Apply(m_ctx.projectRoot);
    runtime.ActivateScriptRuntime(core::Application::Get().GetRenderer(),
        static_cast<uint32_t>(m_ctx.gameViewportWidth), static_cast<uint32_t>(m_ctx.gameViewportHeight));
    m_scriptQuitSceneLifetime = 0;
    ActiveRuntime().SetScriptQuitHandler([](void* context, uint64_t lifetime) {
        static_cast<EditorApp*>(context)->m_scriptQuitSceneLifetime = lifetime;
    }, this);
    /// @note 実行中の品質変更は専用コピーへ限定し、アセットと編集側の自動保存を汚さない。
    (void)asset::ResolveRenderPipelineSettings(m_ctx.projectSettings.render,
        m_ctx.projectSettings.renderPipelineAssetPath, m_playRenderSettings);
    core::Application::Get().SetActiveRenderSettings(&m_playRenderSettings);
    /// @note Play 中の増加も Stop 後の残りも、この 1 つの基準から測る。
    if (m_ctx.resources != nullptr) {
        m_memoryLeakDiff.CaptureBaseline(*m_ctx.resources, "Play");
        /// @note 最初の Play だけ «セッション基準» も置く。キャッシュの初回充填と、
        /// @note        往復のたびに積むリークを、1 往復ぶんの差分だけでは区別できないため。
        m_memoryLeakDiff.CaptureSessionBaselineIfAbsent(*m_ctx.resources);
    }
    if (m_playMode.IsPlaying() && allowPhysicalInput && m_ctx.playFocusMode != EditorContext::PlayFocusMode::Unfocused)
        m_ctx.requestGameViewportFocus = true;
}

void EditorApp::TickPendingShadowBakePlay()
{
    if (!m_pendingShadowBakePlayScene) return;
    if (!m_scene || m_scene->GetRenderSceneGeneration() != m_pendingShadowBakePlayScene) {
        m_pendingShadowBakePlayScene = 0;
        return;
    }
    if (!m_playMode.IsInEditor() || m_ctx.InPrefabEditMode()) {
        scene::CancelDirectionalShadowBake(*m_scene);
        m_pendingShadowBakePlayScene = 0;
        return;
    }
    if (m_packageBuild.IsRunning() || m_ctx.managedBuildBusy) return;

    renderer::RenderSettings settings;
    (void)asset::ResolveRenderPipelineSettings(m_ctx.projectSettings.render,
        m_ctx.projectSettings.renderPipelineAssetPath, settings);
    settings.StripDebugVisualization();
    scene::ProcessDirectionalShadowBake(*m_scene, *m_renderer, *m_resources, settings);
    const auto status = scene::GetDirectionalShadowBakeStatus(*m_scene);
    if (status == scene::DirectionalShadowBakeStatus::PENDING) return;

    m_pendingShadowBakePlayScene = 0;
    if (status == scene::DirectionalShadowBakeStatus::UNAVAILABLE)
        Toast::Warning("Shadow bake unavailable. Play will use realtime shadows.");
    StartPlayMode(true, m_pendingShadowBakeAllowPhysicalInput);
}

void EditorApp::StopPlayMode()
{
    if (m_pendingShadowBakePlayScene) {
        if (m_scene && m_scene->GetRenderSceneGeneration() == m_pendingShadowBakePlayScene)
            scene::CancelDirectionalShadowBake(*m_scene);
        m_pendingShadowBakePlayScene = 0;
        return;
    }
    ResetSpatialDebugCapture();
    if (m_standalonePlay.IsActive()) { m_standalonePlay.Stop(); return; }
    m_scriptQuitSceneLifetime = 0;
    if (!m_ctx.activeScene || m_playMode.IsInEditor())
        return;
    /// @note 実行用コピーを切り離す。ProjectSettings は Play 中も編集側の正本を保持する。
    core::Application::Get().SetActiveRenderSettings(nullptr);
    /// @note AudioSystemはSimOnlyのため、EditModeへ戻った後ではループVoiceを停止できない。
    /// @note        PauseではなくPlay終了時だけ一括停止し、BGMがEditor操作中まで残ることを防ぐ。
    if (auto* audioManager = core::Application::Get().GetAudioManager())
        audioManager->StopAllVoices();
    m_playMode.Stop(*m_ctx.activeScene);
}

void EditorApp::TogglePlayMode(bool allowPhysicalInput)
{
    if (m_playMode.IsInEditor() && !m_standalonePlay.IsActive() && !m_pendingShadowBakePlayScene)
        StartPlayMode(false, allowPhysicalInput);
    else
        StopPlayMode();
}

/// @note ネイティブメニューも描画モード操作の登録簿を使う。
void EditorApp::InvokeViewMode(const char* mode)
{
    OpArgs args;
    args.Set("mode", std::string(mode));
    InvokeOperator("render.set_view_mode", args);
}

/// @note  operator をメニュー項目として描く
/// @note  表示名・ショートカット文字列・実行可否・実体の 4 つとも登録済みの情報から引く。
/// @note  直書きするとリバインドで表示だけが嘘になり、実行可否もキー側と別式になる。
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
    /// @note        描くときだけ。LOC は "訳###原文" を返すので、ID は英語版と同じままになる。
    const char* label = LOC((labelOverride != nullptr) ? labelOverride : op->label.c_str());
    const bool  enabled = CanInvokeOperator(operatorId, args);

    /// @note チェックマークも登録簿から引く。フラグを直接指すと表示と実体が別経路になり、
    /// @note        operator 側に条件を足してもメニューの見た目に反映されない。
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

/// @note  Operator のカテゴリ文字列を、ショートカット一覧の見出し分類へ対応づける。
/// @note  一覧は enum、Operator 側は文字列カテゴリなので、対応表をここに 1 つ置く。
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

/// @note  ホットキー登録 — キー割り当ての単一の定義場所
/// @note  ここに書けば入力処理・F1 の一覧・リバインド UI・設定への永続化が全部ついてくる
/// @note  (パネル側で IsKeyPressed を直接叩くと、その 4 つが揃わない)。
/// @note  「何をするか」「いつ実行できるか」は OperatorRegistry が持ち、ここは operator id に
/// @note  キーを割り当てるだけの表。条件式を各面へ写すと必ずずれる。
/// @note  Docs/design/editor-operator-model.md
void EditorApp::RegisterDefaultHotkeys()
{
    /// @note 説明専用エントリ (RegisterInfo) で使う
    using Cat   = HotkeyCategory;
    using Scope = HotkeyScope;

    /// @note operator id へキーを割り当てる。表示名・分類・実行・実行可否はすべて
    /// @note        レジストリ側から導出するので、ここでキー以外を書くことはない。
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
    /// @note        Unity 同様、選択後にフォーカスを移さず Delete / Ctrl+D を通す。
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
    /// @note        同じ operator への 2 本目なので operatorId は付けない (付けると Rebind が
    /// @note        id で引いたときどちらを指すか決まらない)。保存鍵は表示名になる。
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
    /// @note        ViewportPanel のブックマーク処理も Alt 押下中はスキップし、取り合いを避けている。
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
    /// @note        別表に切り出すとそこがまた二重管理になるので、同じ器に入れて一覧を 1 本に保つ。
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
    /// @note        operator にできない)。入力には関与しない説明専用エントリとして一覧にだけ出す —
    /// @note        一覧に出ないキーは «無い» のと同じで、特に Ctrl+S はここで明示しないと意味が読めない。
    m_hotkeys.RegisterInfo("Save fluid document", "Ctrl+S", Cat::File, Scope::FluidEditor);
    m_hotkeys.RegisterInfo("Play / pause preview", "Space", Cat::Play, Scope::FluidEditor);
    m_hotkeys.RegisterInfo("Step frame",           "Left / Right", Cat::Play, Scope::FluidEditor);
    m_hotkeys.RegisterInfo("Jump to start / end",  "Home / End",   Cat::Play, Scope::FluidEditor);
    m_hotkeys.RegisterInfo("Delete selected part", "Delete",       Cat::Edit, Scope::FluidEditor);
    m_hotkeys.RegisterInfo("Insert motion key at playhead", "K",   Cat::Edit, Scope::FluidEditor);
    m_hotkeys.RegisterInfo("Rename selected part", "F2",           Cat::Edit, Scope::FluidEditor);

    /// @note 保存済みリバインドの適用は EditorApp::OpenProject が行う (設定を読むのがそこ)。
    /// @note        Rebind は既定値を全部積んだ後でないと対象を引けないので、順序はここより後。

    /// @note scope の判定は EditorContext のフォーカス状態から答える。パネルは描画中にしか
    /// @note        自身のフォーカスを知れないため、ここで見るのは 1 フレーム前の状態になる
    /// @note        (キー入力への応答としては問題ない)。
    m_hotkeys.SetScopeResolver([this](HotkeyScope scope) {
        /// @note 「今フォーカスされている面」は 1 つ (EditorContext::focusedPanelScope)。
        /// @note        パネルが増えてもここは変わらない — 各パネルが IPanel::GetHotkeyScope() で名乗る。
        if (m_ctx.PanelScopeFocused(scope)) return true;

        /// @note Scene View だけはホバーでも効かせる。ギズモ切替やカメラ操作は «絵を見ながら»
        /// @note        押すもので、先にクリックしてフォーカスを取る手順を挟むと手が止まる。
        /// @note        他の面はフォーカスのみで判定する方が読める。
        return HasScope(scope, HotkeyScope::SceneViewport) && m_ctx.sceneViewportHovered;
    });
}

}
