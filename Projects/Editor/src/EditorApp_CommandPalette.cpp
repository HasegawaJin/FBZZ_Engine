// FBZZ Engine
// EditorApp_CommandPalette.cpp | fbzz::editor
// コマンドパレット (Ctrl+K): アクションを名前で検索して即実行するクイックランチャー
//
// WHY: パネル・メニュー階層が多く、目的の操作へ到達する導線が深い。VSCode / Unity の
//      コマンドパレット同様、キーボードだけで検索→実行できる横断的な入口を用意して発見性を上げる。
#include <Editor/EditorApp.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Math/Vector3.hpp>
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace fbzz::editor {

void EditorApp::RefreshPaletteAssetIndex()
{
    m_paletteAssetPaths.clear();
    if (m_ctx.projectRoot.empty()) return;

    const std::filesystem::path assetsDir =
        util::FileSystem::PathFromUtf8(m_ctx.projectRoot) / L"Assets";
    if (!util::FileSystem::Exists(assetsDir)) return;

    // ユーザーが「開きたい」主要アセットのみ索引する。中間/生成物 (.meta 等) はノイズなので除外。
    static const std::unordered_set<std::string> kIncludeExt = {
        ".scene", ".prefab", ".mat", ".fbx", ".obj", ".gltf", ".glb",
        ".png", ".jpg", ".jpeg", ".tga", ".dds", ".hdr",
        ".hlsl", ".hlsli", ".hpp", ".cpp", ".h", ".cs", ".lua",
        ".anim", ".animcontroller", ".vfx", ".skel",
        ".wav", ".mp3", ".ogg", ".terrain", ".asset", ".fzdata", ".physmat",
    };
    for (const auto& p : util::FileSystem::ListFilesRecursive(assetsDir)) {
        const std::string ext = util::StringUtils::ToLower(
            util::FileSystem::PathToUtf8(p.extension()));
        if (kIncludeExt.find(ext) == kIncludeExt.end()) continue;
        m_paletteAssetPaths.push_back(util::FileSystem::PathToUtf8(p));
        if (m_paletteAssetPaths.size() >= 5000) break; // 暴走防止の上限
    }
}

void EditorApp::DrawCommandPalette(EditorContext& ctx)
{
    constexpr const char* kPopupId = "##command_palette";

    // ホットキー / メニューからの起動要求を受けてポップアップを開く。
    if (m_commandPaletteOpen) {
        m_commandPaletteOpen = false;
        m_commandPaletteQuery[0] = '\0';
        m_commandPaletteSel = 0;
        m_commandPaletteFocus = true;
        RefreshPaletteAssetIndex(); // 開いた瞬間のアセット一覧をスナップショット
        ImGui::OpenPopup(kPopupId);
    }

    // 画面上部中央に配置する。
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        { vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.22f },
        ImGuiCond_Appearing, { 0.5f, 0.0f });
    ImGui::SetNextWindowSize({ 520.0f, 0.0f }, ImGuiCond_Appearing);

    if (!ImGui::BeginPopup(kPopupId, ImGuiWindowFlags_NoMove))
        return;

    // ── コマンド一覧を毎フレーム構築する ─────────────────────────────────────
    // WHY: Undo ラベルやパネル表示状態など動的な要素を含むため、開いている間に作り直す。
    struct Command {
        std::string           category;
        std::string           label;
        std::function<void()> action;
        bool                  enabled = true;
    };
    std::vector<Command> cmds;
    const auto add = [&](const char* cat, std::string label,
                         std::function<void()> fn, bool enabled = true) {
        cmds.push_back({ cat, std::move(label), std::move(fn), enabled });
    };

    const bool hasScene = ctx.activeScene != nullptr;
    const bool inEditor = ctx.playMode && ctx.playMode->IsInEditor();

    // クエリが空のときは Recent Scenes を先頭に提示し、素早い再オープンを可能にする。
    if (m_commandPaletteQuery[0] == '\0') {
        for (const std::string& sp : m_settings.recentScenes) {
            const bool exists = util::FileSystem::Exists(sp);
            add("Recent", util::FileSystem::GetFilename(sp),
                [this, sp] { RequestOpenScenePath(sp); }, exists);
        }
    }

    // File
    add("File", "New Scene", [this] { RequestNewScene(); });
    add("File", "Open Scene...", [this] { RequestOpenSceneFromDialog(); }, hasScene);
    add("File", "Save Scene", [this] { SaveScene(); }, hasScene);
    add("File", "Save Scene As...", [this] { SaveSceneAsDialog(); }, hasScene);
    add("File", "Save All Assets", [] { AssetDirtyRegistry::SaveAll(); });
    // Edit
    add("Edit", "Undo", [this] { m_undoStack.Undo(); }, m_undoStack.CanUndo());
    add("Edit", "Redo", [this] { m_undoStack.Redo(); }, m_undoStack.CanRedo());
    // Play
    add("Play", inEditor ? "Play" : "Stop", [this] { TogglePlayMode(); }, hasScene);
    add("Play", "Reload Scripts", [&ctx] { ctx.requestScriptReload = true; });
    // Tools / Windows
    add("Tools", "Map Editing Mode", [&ctx] { ctx.requestMapEditingModeToggle = true; }, hasScene && inEditor);
    add("Tools", "Build Settings...", [&ctx] { ctx.requestOpenBuildSettings = true; });
    add("Tools", "Project Settings...", [&ctx] { ctx.requestOpenProjectSettings = true; });
    add("Tools", "Analysis", [&ctx] { ctx.requestOpenAnalysis = true; });
    add("Tools", "Terrain Tool", [&ctx] { ctx.showTerrainTool = true; });
    add("Tools", "Water Tool", [&ctx] { ctx.showWaterTool = true; });
    add("Tools", "Detail Tool", [&ctx] { ctx.showDetailTool = true; });
    add("Tools", "Foliage Tool", [&ctx] { ctx.showFoliageTool = true; });
    // Windows: 各パネルの表示トグル
    for (auto& panel : m_panels) {
        if (!panel->ShowInViewMenu()) continue;
        IPanel* pp = panel.get();
        const bool visible = pp->visible;
        std::string label = std::string(visible ? "Hide: " : "Show: ") + panel->GetViewMenuName();
        add("Window", std::move(label), [pp] { pp->visible = !pp->visible; });
    }

    // ── 検索欄 ───────────────────────────────────────────────────────────────
    if (m_commandPaletteFocus) {
        ImGui::SetKeyboardFocusHere();
        m_commandPaletteFocus = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    const bool entered = ImGui::InputTextWithHint(
        "##cmd_query", "Type a command...  (Enter to run, Esc to close)",
        m_commandPaletteQuery, sizeof(m_commandPaletteQuery),
        ImGuiInputTextFlags_EnterReturnsTrue);

    // フィルタ (部分一致・大小無視)。ラベルとカテゴリの両方を対象にする。
    const std::string query = m_commandPaletteQuery;

    // ── Go to Anything: クエリ入力時のみアセット / GameObject を動的に候補追加する ──
    // WHY: 空クエリで全アセット/全オブジェクトを並べると膨大になるため、絞り込み時だけ列挙する。
    if (!query.empty()) {
        // アセット (ファイル名の部分一致)。シーンは開く、それ以外は選択して Inspector に表示。
        int assetHits = 0;
        for (const std::string& path : m_paletteAssetPaths) {
            if (assetHits >= 50) break; // 候補が溢れないよう上限を設ける
            const std::string name = util::FileSystem::GetFilename(path);
            if (!util::StringUtils::ContainsCI(name, query)) continue;
            ++assetHits;
            const std::string ext = util::StringUtils::ToLower(util::FileSystem::GetExtension(path));
            add("Asset", name, [this, path, ext, &ctx] {
                if (ext == ".scene") {
                    RequestOpenScenePath(path);
                } else if (ext == ".animcontroller") {
                    ctx.selectedAssetPath = path;
                    ctx.requestOpenAnimationGraph = true;
                } else if (ext == ".vfx") {
                    ctx.selectedAssetPath = path;
                    ctx.requestOpenVFXEditor = true;
                } else {
                    ctx.selectedAssetPath = path; // Inspector にアセットを表示
                }
            });
        }
        // GameObject (名前の部分一致)。選択してビューをフォーカスする。
        if (ctx.activeScene) {
            int goHits = 0;
            for (auto& go : ctx.activeScene->GameObjects()) {
                if (goHits >= 50) break;
                if (!util::StringUtils::ContainsCI(go.name, query)) continue;
                ++goHits;
                const scene::EntityID id = go.GetID();
                const math::Vector3   wp = go.transform.worldPosition;
                add("Object", go.name, [id, wp, &ctx] {
                    ctx.selectedEntities       = { id };
                    ctx.focusTargetPosition    = wp;
                    ctx.focusTargetRadius      = 0.0f;   // バウンズ不明時は既定距離フォーカス
                    ctx.requestFocusOnSelected = true;
                });
            }
        }
    }

    std::vector<int> filtered;
    filtered.reserve(cmds.size());
    for (int i = 0; i < static_cast<int>(cmds.size()); ++i) {
        if (query.empty()
            || util::StringUtils::ContainsCI(cmds[i].label, query)
            || util::StringUtils::ContainsCI(cmds[i].category, query))
            filtered.push_back(i);
    }

    // 選択インデックスをクランプし、上下キーで移動する。
    if (filtered.empty())
        m_commandPaletteSel = 0;
    else {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
            m_commandPaletteSel = (m_commandPaletteSel + 1) % static_cast<int>(filtered.size());
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            m_commandPaletteSel = (m_commandPaletteSel - 1 + static_cast<int>(filtered.size()))
                                  % static_cast<int>(filtered.size());
        m_commandPaletteSel = std::clamp(m_commandPaletteSel, 0, static_cast<int>(filtered.size()) - 1);
    }

    // 実行ヘルパー。
    const auto runSelected = [&]() {
        if (m_commandPaletteSel < 0 || m_commandPaletteSel >= static_cast<int>(filtered.size()))
            return;
        const Command& c = cmds[filtered[m_commandPaletteSel]];
        if (c.enabled && c.action) c.action();
        ImGui::CloseCurrentPopup();
    };

    ImGui::Separator();

    // ── 結果リスト ───────────────────────────────────────────────────────────
    const float listH = std::min(320.0f, ImGui::GetTextLineHeightWithSpacing() * 12.0f);
    ImGui::BeginChild("##cmd_list", { 0.0f, listH }, false);
    for (int row = 0; row < static_cast<int>(filtered.size()); ++row) {
        const Command& c = cmds[filtered[row]];
        const bool selected = (row == m_commandPaletteSel);
        ImGui::PushID(row);
        if (!c.enabled) ImGui::BeginDisabled();

        // "カテゴリ  ラベル" を1行で表示。選択行は Selectable のハイライトで示す。
        char rowLabel[256];
        std::snprintf(rowLabel, sizeof(rowLabel), "%-8s  %s", c.category.c_str(), c.label.c_str());
        if (ImGui::Selectable(rowLabel, selected)) {
            m_commandPaletteSel = row;
            if (c.enabled && c.action) c.action();
            ImGui::CloseCurrentPopup();
        }
        if (!c.enabled) ImGui::EndDisabled();

        // 選択行が見えるようスクロール追従する。
        if (selected && (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)))
            ImGui::SetScrollHereY(0.5f);
        ImGui::PopID();
    }
    if (filtered.empty())
        ImGui::TextDisabled("  No matching commands");
    ImGui::EndChild();

    // Enter (入力欄でも) で選択実行、Esc で閉じる。
    if (entered) runSelected();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

} // namespace fbzz::editor
