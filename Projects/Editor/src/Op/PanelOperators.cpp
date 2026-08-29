/// @file    PanelOperators.cpp
/// @brief   ワークスペース (パネル表示 / UI スケール / Prefab 編集モード) の Operator。
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY: これらは「二重管理が無いから Operator にしない」として意図的に外してあった。
/// パネルの表示トグルは m_panels という単一の出所から View メニューとコマンド
/// パレットが導出しているので、確かに実装は重複していない。
/// しかしその判断は **人が使う 2 面しか数えていなかった**。AI から見ると、
/// パネルは「存在すら列挙できない対象」で、開くことも閉じることもできない。
/// viewport_capture が撮るのは Scene / Game の RT なので、Console や Inspector の
/// 中身は撮れず、「今どのパネルが開いているか」を知る手段も無い。
/// 重複が無いことは Operator にしない理由になるが、**AI から到達できない理由には
/// ならない**。ここでは実装を複製せず、m_panels をそのまま引数で引ける
/// 1 つの操作として公開する (パネルが増えても操作は増えない)。
///
/// 設計: Docs/design/editor-operator-model.md §7
#include <Editor/EditorApp.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Engine/Util/StringUtils.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::editor {

namespace {

// UI スケールの許容範囲。**ここが唯一の定義場所**で、View メニューのスライダーは
// operator の params 宣言 (hasRange / minValue / maxValue) から読み取る。
// WHY: スライダー側にも書くと、片方だけ広げたときに「人は 0.5x にできるのに
//      AI からは BAD_ARG で弾かれる」= 同じ操作の限界が面ごとに違う状態になり、
//      この設計が消したいはずのずれが範囲という形で再発する。
constexpr float kUiScaleMin = 0.7f;
constexpr float kUiScaleMax = 2.0f;

} // namespace

// パネルを名前で引く。ウィンドウ名 → View メニュー名の順に、大小無視の完全一致。
// WHY 部分一致にしないか: "Console" が "Build Output Console" にも当たると、
//     AI は当てたつもりの無いパネルを閉じる。候補は panel.list で全部返しているので、
//     曖昧一致で救う必要がない。
IPanel* EditorApp::FindPanelByName(const std::string& name) const
{
    for (const auto& panel : m_panels)
        if (util::StringUtils::EqualsCI(panel->GetWindowName(), name)) return panel.get();
    for (const auto& panel : m_panels)
        if (util::StringUtils::EqualsCI(panel->GetViewMenuName(), name)) return panel.get();
    return nullptr;
}

void EditorApp::CaptureNormalPanelVisibility()
{
    // Map Mode / Play Maximized 中は panel->visible がその一時レイアウト用に潰されている。
    // 入る前に控えたスナップショットがあるなら、そちらが「通常の開閉状態」。
    const std::vector<bool>* snapshot = nullptr;
    if (m_playViewportLayoutActive && m_playPanelVisibility.size() == m_panels.size())
        snapshot = &m_playPanelVisibility;
    else if (m_ctx.mapEditingMode && m_normalPanelVisibility.size() == m_panels.size())
        snapshot = &m_normalPanelVisibility;

    m_settings.panelVisibility.clear();
    for (std::size_t index = 0; index < m_panels.size(); ++index) {
        const IPanel& panel = *m_panels[index];
        // View > Panels に出ないパネル (Build Settings / IBL Bake / Map Editor) は
        // 開閉が操作や編集モードに従属する。復元すると自分で開いた覚えの無い窓が出る。
        if (!panel.ShowInViewMenu()) continue;
        const bool open = snapshot ? (*snapshot)[index] : panel.visible;
        m_settings.panelVisibility.emplace_back(panel.GetWindowName(), open);
    }
}

void EditorApp::RestorePanelVisibility()
{
    if (m_settings.panelVisibility.empty()) return;

    for (auto& panel : m_panels) {
        if (!panel->ShowInViewMenu()) continue;
        const std::string name = panel->GetWindowName();
        const auto it = std::find_if(
            m_settings.panelVisibility.begin(), m_settings.panelVisibility.end(),
            [&name](const auto& entry) { return entry.first == name; });
        // 保存に無いパネル (このバージョンで増えたもの) は既定の表示のままにする。
        if (it != m_settings.panelVisibility.end())
            panel->visible = it->second;
    }
}

void EditorApp::InvokePanelFocus(IPanel* panel)
{
    if (panel == nullptr) return;
    OpArgs args;
    args.Set("panel", std::string(panel->GetWindowName()));
    InvokeOperator("panel.focus", args);
}

void EditorApp::RegisterPanelOperators()
{
    // ── パネルの目録 (Query) ────────────────────────────────────────────────
    // WHY Query が要るか: 名前を知らなければ panel.set_visible は呼べない。
    //     ウィンドウ名は表示ラベルと違うことがある (View メニュー名は別に持てる)
    //     ので、AI が画面の文言から推測すると外す。
    {
        EditorOperator op;
        op.id       = "panel.list";
        op.label    = "List Panels";
        op.category = "Panels";
        op.desc     = "エディターパネルの一覧と表示状態を返す。name を panel.set_visible / "
                      "panel.focus の panel 引数へそのまま渡せる。";
        op.kind     = OpKind::Query;
        op.exec = [this](OpContext&, const OpArgs&) -> OpResult {
            OpData panels = OpData::MakeArray();
            for (const auto& panel : m_panels) {
                OpData entry = OpData::MakeObject();
                entry.Set("name", OpData(std::string(panel->GetWindowName())));
                entry.Set("label", OpData(std::string(panel->GetViewMenuName())));
                entry.Set("visible", OpData(panel->visible));
                // visible と「実際に描かれたか」は別物。ドッキングされたタブが
                // 非アクティブなら visible は true のまま中身は 1 px も出ない。
                // 見えている前提で撮ると、無い画を探すことになる。
                entry.Set("contentRendered", OpData(panel->WasContentRendered()));
                entry.Set("inViewMenu", OpData(panel->ShowInViewMenu()));
                panels.Push(std::move(entry));
            }
            OpData data = OpData::MakeObject();
            data.Set("count", OpData(static_cast<int>(m_panels.size())));
            data.Set("panels", std::move(panels));
            return OpResult::Data(std::move(data));
        };
        m_operators.Register(std::move(op));
    }

    // ── 表示の切り替え ──────────────────────────────────────────────────────
    {
        EditorOperator op;
        op.id       = "panel.set_visible";
        op.label    = "Show / Hide Panel";
        op.category = "Panels";
        op.desc     = "パネルの表示・非表示を切り替える。visible を省略すると反転する。";
        op.kind     = OpKind::Action;

        OpParam panelParam;
        panelParam.name = "panel";
        panelParam.type = OpParamType::String;
        panelParam.desc = "panel.list が返す name (または View メニュー名)";
        OpParam visibleParam;
        visibleParam.name     = "visible";
        visibleParam.type     = OpParamType::Bool;
        visibleParam.desc     = "省略すると現在値を反転する";
        visibleParam.required = false;
        op.params = { panelParam, visibleParam };

        op.poll = [this](const OpContext&, const OpArgs& args) {
            // 引数なしの評価 (メニュー / パレット) では「操作自体は使える」と答える。
            // 対象が決まらないうちに false を返すと、パレットで常に淡色表示になる。
            if (!args.Has("panel")) return true;
            return FindPanelByName(args.GetString("panel")) != nullptr;
        };
        op.checked = [this](const OpContext&, const OpArgs& args) {
            const IPanel* panel = FindPanelByName(args.GetString("panel"));
            return panel != nullptr && panel->visible;
        };
        op.exec = [this](OpContext&, const OpArgs& args) -> OpResult {
            const std::string name  = args.GetString("panel");
            IPanel*           panel = FindPanelByName(name);
            if (panel == nullptr)
                return OpResult::Err("UNKNOWN_PANEL",
                                     "そのパネルはありません: " + name + " (panel.list で一覧できます)");

            const bool next = args.Has("visible") ? args.GetBool("visible") : !panel->visible;
            OpResult result;
            result.noChange = (next == panel->visible);
            panel->visible  = next;
            result.message  = next ? "表示しました" : "非表示にしました";
            return result;
        };
        m_operators.Register(std::move(op));
    }

    // ── フォーカス ──────────────────────────────────────────────────────────
    // WHY set_visible と分けるか: 表示済みでもドッキングされたタブが背面だと
    //     中身は描かれない (IPanel::WasContentRendered)。「開いたのに映らない」の
    //     直し方が「もう一度 visible=true にする」ではないので、別の操作にする。
    {
        EditorOperator op;
        op.id       = "panel.focus";
        op.label    = "Focus Panel";
        op.category = "Panels";
        op.desc     = "パネルを表示し、ドッキングされたタブを前面へ出す。"
                      "visible=true でも背面タブだと中身は描かれないため、"
                      "内容を見せたいときはこちらを使う。";
        op.kind     = OpKind::Action;

        OpParam panelParam;
        panelParam.name = "panel";
        panelParam.type = OpParamType::String;
        panelParam.desc = "panel.list が返す name";
        op.params = { panelParam };

        op.poll = [this](const OpContext&, const OpArgs& args) {
            if (!args.Has("panel")) return true;
            return FindPanelByName(args.GetString("panel")) != nullptr;
        };
        op.exec = [this](OpContext&, const OpArgs& args) -> OpResult {
            const std::string name  = args.GetString("panel");
            IPanel*           panel = FindPanelByName(name);
            if (panel == nullptr)
                return OpResult::Err("UNKNOWN_PANEL",
                                     "そのパネルはありません: " + name + " (panel.list で一覧できます)");
            panel->visible = true;
            // AI バスの drain は ImGui::NewFrame の後 (EditorApp::OnUpdate) なので、
            // ここから ImGui を呼んでよい。次のフレームまで持ち越す必要がない。
            ImGui::SetWindowFocus(panel->GetWindowName());
            return OpResult::Ok();
        };
        m_operators.Register(std::move(op));
    }

    // ── UI スケール ─────────────────────────────────────────────────────────
    // WHY AI に要るか: viewport_capture ではなくエディター全体のスクリーンショットを
    //     読むとき、既定スケールでは文字が潰れて読めないことがある。
    //     View メニューのスライダーにしか無いと、AI は撮り直す手段を持たない。
    {
        EditorOperator op;
        op.id       = "view.set_ui_scale";
        op.label    = "Set UI Scale";
        op.category = "Viewport";
        op.desc     = "エディター UI 全体 (フォントと余白) の倍率を設定する。設定に永続化される。";
        op.kind     = OpKind::Action;

        OpParam scaleParam;
        scaleParam.name     = "scale";
        scaleParam.type     = OpParamType::Float;
        scaleParam.desc     = "倍率";
        scaleParam.hasRange = true;
        scaleParam.minValue = kUiScaleMin;
        scaleParam.maxValue = kUiScaleMax;
        op.params = { scaleParam };

        op.exec = [this](OpContext& c, const OpArgs& args) -> OpResult {
            const float scale = args.GetFloat("scale", 1.0f);
            OpResult result;
            result.noChange   = (scale == c.ctx.editorUiScale);
            c.ctx.editorUiScale = scale;
            EditorTheme::SetUiScale(scale);
            return result;
        };
        m_operators.Register(std::move(op));
    }

    {
        EditorOperator op;
        op.id       = "view.reset_ui_scale";
        op.label    = "Reset UI Scale";
        op.category = "Viewport";
        op.desc     = "UI 倍率を 1.0x へ戻す。";
        op.kind     = OpKind::Action;
        op.poll = [](const OpContext& c, const OpArgs&) { return c.ctx.editorUiScale != 1.0f; };
        op.exec = [](OpContext& c, const OpArgs&) -> OpResult {
            c.ctx.editorUiScale = 1.0f;
            EditorTheme::SetUiScale(1.0f);
            return OpResult::Ok();
        };
        m_operators.Register(std::move(op));
    }

    // ── Prefab 編集モードの離脱 ─────────────────────────────────────────────
    // WHY: File メニューに「Close Prefab」があるのに operator が無く、
    //      Prefab 編集中は scene.open / scene.new が poll で拒否されるため、
    //      AI が Prefab 編集へ入ると **出る手段が 1 つも無い** 状態だった。
    //      (要求は ProcessPrefabEditRequests がフレーム先頭で処理する。
    //       パネル描画の途中でシーンを差し替えると、以降のパネルが
    //       破棄済みの GameObject を掴むため、ここでは要求だけを立てる。)
    {
        EditorOperator op;
        op.id       = "prefab.close";
        op.label    = "Close Prefab";
        op.category = "File";
        op.desc     = "Prefab 編集モードを抜けて元のシーンへ戻る。";
        op.caution  = "保存していない Prefab の変更は失われる (先に scene.save で保存する)。";
        op.kind     = OpKind::Action;
        op.poll = [](const OpContext& c, const OpArgs&) { return c.ctx.InPrefabEditMode(); };
        op.exec = [](OpContext& c, const OpArgs&) -> OpResult {
            c.ctx.requestClosePrefabEdit = true;
            return OpResult::Ok();
        };
        m_operators.Register(std::move(op));
    }
}

} // namespace fbzz::editor
