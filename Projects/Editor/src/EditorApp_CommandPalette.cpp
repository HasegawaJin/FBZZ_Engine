/// @file    EditorApp_CommandPalette.cpp
/// @brief   コマンドパレット (Ctrl+K): アクションを名前で検索して即実行するクイックランチャー。
/// @author  Hasegawa Jin
/// @date    2026-07-20
///
/// WHY: パネル・メニュー階層が多く、目的の操作へ到達する導線が深い。VSCode / Unity の
/// コマンドパレット同様、キーボードだけで検索→実行できる横断的な入口を用意して発見性を上げる。
#include <Editor/EditorApp.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/Localization.hpp>
#include <Editor/Util/Selection.hpp>
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
        ".anim", ".animcontroller", ".vfx", ".skel", ".sequence", ".behaviortree",
        ".wav", ".mp3", ".ogg", ".flac", ".synth",
        ".terrain", ".asset", ".fzdata", ".physmat",
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
    // WHY: 候補にはパネル表示状態や検索クエリ由来の動的な要素が混ざるため、
    //      開いている間に作り直す。ただし「操作」そのものは OperatorRegistry が
    //      正本で、ここではそれを列挙するだけ (以前はメニュー・ホットキーと
    //      同じ操作をこのファイルへ 3 度目に手書きしており、実行可否の式が
    //      面ごとにずれていた)。Docs/design/editor-operator-model.md
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

    // クエリが空のときは Recent Scenes を先頭に提示し、素早い再オープンを可能にする。
    if (m_commandPaletteQuery[0] == '\0') {
        for (const std::string& sp : m_settings.recentScenes) {
            const bool exists = util::FileSystem::Exists(sp);
            add("Recent", util::FileSystem::GetFilename(sp),
                [this, sp] { RequestOpenScenePath(sp); }, exists);
        }
    }

    // ── 登録済みの操作 ──────────────────────────────────────────────────────
    // 表示名・分類・実行可否・実体はすべてレジストリ側にある。
    // ここに操作を書き足すことはない (足すなら BuiltinOperators.cpp へ)。
    {
        const OpContext opContext = MakeOpContext();
        // パレットは引数を渡せないので、poll も引数なしで評価する。
        // 対象を引数で指定する操作は「選択中のもの」を暗黙の対象として判定される。
        const OpArgs noArgs;
        for (const EditorOperator& op : m_operators.All()) {
            // Query は状態を読むだけで、パレットから実行する意味がない (AI 用)。
            if (op.kind == OpKind::Query) continue;

            // 必須引数を持つ操作は、値を決めずに並べても押した瞬間 BAD_ARG になる。
            // WHY 例外を 1 つ設けるか: 必須引数が 1 つで、取りうる値が宣言されている
            //     (enumValues) 排他選択は、値ごとに 1 行へ展開すれば普通の操作として扱える。
            //     View Mode の 4 つはこれに当たり、以前は「押せるのに必ず失敗する
            //     1 行」としてパレットに並んでいた。
            const std::vector<OpParam>& params = op.params;
            const bool singleEnumParam =
                params.size() >= 1 && params[0].required && !params[0].enumValues.empty()
                && std::none_of(params.begin() + 1, params.end(),
                                [](const OpParam& p) { return p.required; });

            if (singleEnumParam) {
                const std::string id        = op.id;
                const std::string paramName = params[0].name;
                for (const std::string& value : params[0].enumValues) {
                    OpArgs args;
                    args.Set(paramName, value);
                    const bool enabled = !op.poll || op.poll(opContext, args);
                    add(op.category.c_str(), op.label + ": " + value,
                        [this, id, paramName, value] {
                            OpArgs invokeArgs;
                            invokeArgs.Set(paramName, value);
                            InvokeOperator(id, invokeArgs);
                        },
                        enabled);
                }
                continue;
            }

            const bool hasRequiredParam = std::any_of(
                params.begin(), params.end(), [](const OpParam& p) { return p.required; });
            if (hasRequiredParam) continue;

            const std::string id = op.id;
            add(op.category.c_str(), op.label,
                [this, id] { InvokeOperator(id); },
                !op.poll || op.poll(opContext, noArgs));
        }
    }

    // Windows: 各パネルの表示トグル
    // WHY レジストリを列挙するのとは別に並べるか: 対象がパネルごとに変わる
    //     引数付きの操作なので、1 行では「どのパネルか」を指定できない。
    //     一覧の出所は m_panels のままで、実行だけを panel.set_visible へ通す
    //     (人がここから閉じたときと AI が閉じたときで実体が同じになる)。
    for (auto& panel : m_panels) {
        if (!panel->ShowInViewMenu()) continue;
        const bool        visible   = panel->visible;
        const std::string panelName = panel->GetWindowName();
        std::string label = std::string(visible ? "Hide: " : "Show: ") + panel->GetViewMenuName();
        add("Window", std::move(label), [this, panelName] {
            OpArgs args;
            args.Set("panel", panelName);
            InvokeOperator("panel.set_visible", args);
        });
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
        // アセット (ファイル名の部分一致)。開き方の振り分けは asset.open が持つ。
        // WHY 拡張子の分岐をここに書かないか: 同じ振り分けが AssetBrowser の
        //     ダブルクリック・SearchEverything・ここの 3 箇所へ写されており、
        //     .behaviortree はこのパレットからだけ開けない (分岐が抜けている) 状態だった。
        //     operator へ寄せれば、対応拡張子を足したときに全経路へ同時に効く。
        int assetHits = 0;
        for (const std::string& path : m_paletteAssetPaths) {
            if (assetHits >= 50) break; // 候補が溢れないよう上限を設ける
            const std::string name = util::FileSystem::GetFilename(path);
            if (!util::StringUtils::ContainsCI(name, query)) continue;
            ++assetHits;
            add("Asset", name, [this, path] {
                OpArgs args;
                args.Set("path", path);
                InvokeOperator("asset.open", args);
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
                    SelectEntity(ctx, id);
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
        // 原文と訳の両方に当てる。日本語表示中でも英語の操作名で引けること ──
        // 資料もログも英語で、覚えているのが "Frame Selected" のときに引けないと、
        // パレットは «知っている名前で呼ぶ» 道具でなくなる。
        if (query.empty()
            || util::StringUtils::ContainsCI(cmds[i].label, query)
            || util::StringUtils::ContainsCI(LOCT(cmds[i].label.c_str()), query)
            || util::StringUtils::ContainsCI(cmds[i].category, query)
            || util::StringUtils::ContainsCI(LOCT(cmds[i].category.c_str()), query))
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

        // "カテゴリ  ラベル" を 1 行で表示。選択行は Selectable のハイライトで示す。
        //
        // WHY 桁を "%-8s" で揃えないか: 空白詰めはバイト数で効くので、1 文字 3 バイトの
        //     日本語では «AI» だけが 6 個も詰められ、カテゴリ列の右端が行ごとに動く。
        //     文字数でもなく px で列を決めれば、どの言語でもラベルの左端がそろう。
        const float catWidth = ImGui::GetFontSize() * 5.5f;
        const float gap      = ImGui::GetFontSize() * 0.6f;
        const ImVec2 rowPos  = ImGui::GetCursorScreenPos();

        // 当たり判定は行全体。文字は上から重ねて描く。
        if (ImGui::Selectable("##row", selected)) {
            m_commandPaletteSel = row;
            if (c.enabled && c.action) c.action();
            ImGui::CloseCurrentPopup();
        }

        const float labelWidth = std::max(ImGui::GetContentRegionAvail().x - catWidth - gap,
                                          ImGui::GetFontSize() * 4.0f);
        const ImU32 catColor   = EditorTheme::ColorU32(
            ThemeColor::TextFaint, c.enabled ? 1.0f : 0.5f);
        const ImU32 labelColor = EditorTheme::ColorU32(
            c.enabled ? ThemeColor::Text : ThemeColor::TextFaint, c.enabled ? 1.0f : 0.6f);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddText(rowPos, catColor,
                          widgets::ElideToWidth(LOCT(c.category.c_str()), catWidth).c_str());
        drawList->AddText({ rowPos.x + catWidth + gap, rowPos.y }, labelColor,
                          widgets::ElideToWidth(LOCT(c.label.c_str()), labelWidth).c_str());
        if (!c.enabled) ImGui::EndDisabled();

        // 選択行が見えるようスクロール追従する。
        if (selected && (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)))
            ImGui::SetScrollHereY(0.5f);
        ImGui::PopID();
    }
    if (filtered.empty())
        ImGui::TextDisabled("  %s", LOCT("No matching commands"));
    ImGui::EndChild();

    // Enter (入力欄でも) で選択実行、Esc で閉じる。
    if (entered) runSelected();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

} // namespace fbzz::editor
