/// @file    FluidEditorPanel.hpp
/// @brief   .fluid レシピを編集する専用パネル (Outliner・2D ライブプレビュー・タイムライン・Properties)
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// WHY 専用パネルにするか:
///   Inspector の縦 1 列では «どの部品がいつ・どこで効くか» が見えず、値を 1 つ触るたびに頭から解き直していた。
///   部品を絵の上で掴んで動かし、タイムラインで出番をずらし、スクラブで同じ瞬間を見比べる、という
///   EmberGen / Niagara と同じ手順をそのまま UI にする。
///
/// 開くのは asset.open (Asset Browser のダブルクリック) が積む EditorContext::requestOpenFluidEditor。
/// 要求はこのパネルが OnBeforeBegin で読んで消す (背面タブのときも読めるように Begin の前で読む)。
#pragma once
#include <Editor/Panels/IPanel.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::editor::fluideditor { struct State; struct FluidSaveAsResult; }

namespace fbzz::editor {

class FluidEditorPanel final : public IPanel {
public:
    FluidEditorPanel();
    ~FluidEditorPanel() override;
    FluidEditorPanel(const FluidEditorPanel&) = delete;
    FluidEditorPanel& operator=(const FluidEditorPanel&) = delete;

    const char* GetWindowName()        const override { return "Fluid Editor"; }
    // ここにフォーカスがある間、Ctrl+S は «開いている .fluid を保存» になる。
    // 何も開いていないときは名乗らない (空の Fluid Editor にフォーカスがあるだけで
    // シーンが保存できなくなるため)。判定に FluidDocument が要るので実装は .cpp。
    HotkeyScope GetHotkeyScope()       const override;
    const char* GetMenuCategory()      const override { return "Tools"; }
    bool        GetDefaultVisibility() const override { return false; }

    void OnInit(EditorContext& ctx) override;
    void OnShutdown() override;
    void OnLoadSettings(const EditorSettings& settings) override;
    void OnSaveSettings(EditorSettings& settings) const override;

protected:
    void OnBeforeBegin(EditorContext& ctx) override;
    void OnRenderContent(EditorContext& ctx) override;
    ImGuiWindowFlags GetWindowFlags() const override
    {
        return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    }

private:
    /// 未保存の変更があれば Save / Discard / Cancel を尋ねてから開く。
    void RequestOpen(EditorContext& ctx, const std::string& absPath);
    void OpenNow(EditorContext& ctx, const std::string& absPath);
    /// Asset Browser で選んだ .fluid へ編集対象を移す。未保存の変更があるときは移さない。
    void FollowAssetSelection(EditorContext& ctx);
    /// 履歴の先頭へ置く (同じ相手は 1 本にまとめ、古い方から溢れさせる)。
    void RememberRecent(EditorContext& ctx, const std::string& absPath);
    /// 履歴から外す (実体が無くなっていた相手を選んだとき)。
    void ForgetRecent(const std::string& absPath);
    /// テンプレートなら «名前を付けて保存» へ倒し、書かずに false を返す。
    bool SaveDocument(EditorContext& ctx);
    /// 置き場を問わずディスクへ書く (テンプレートの上書きを人が選んだときの口)。
    bool WriteDocumentToDisk(EditorContext& ctx);
    void BeginSaveAs(EditorContext& ctx);
    void FinishSaveAs(EditorContext& ctx, const fluideditor::FluidSaveAsResult& result);
    void BeginNewFromPreset(EditorContext& ctx);
    void CreateFromPreset(EditorContext& ctx, const std::string& rawName);
    void StartBake(EditorContext& ctx, bool makeMaterial, bool makeVfx);
    void PollBakeJob(EditorContext& ctx);
    void SyncDirtyRegistry(EditorContext& ctx);
    void SyncPreview();
    void AdvancePlayback();
    void HandleShortcuts(EditorContext& ctx);

    void DrawToolbar(EditorContext& ctx);
    void DrawEffectTemplates(EditorContext& ctx);
    /// Open ▾ (Recent / New... / Preset...)。押した結果はポップアップを閉じてから効かせる。
    void DrawOpenMenu(EditorContext& ctx);
    /// Recent の一覧を Open ▾ の中へ並べる。選んだ相手は picked へ入れて呼び手に任せる
    /// (開く経路は確認モーダルを開きうるので、ポップアップを描いている最中には走らせない)。
    void DrawRecentEntries(EditorContext& ctx, std::string& picked);
    /// Recent で選んだ相手を開く。実体が無くなっていたら理由を出して履歴から外す。
    void OpenRecent(EditorContext& ctx, const std::string& picked);
    /// ⚙ (Auto Save / Follow Selection / Quality)。品質だけは «今どれか» を隣に出す。
    void DrawSettingsMenu(EditorContext& ctx);
    /// 2 行目。反復中に読む値 (コマ番号・時刻・解けた所・焼かれる形) と、状態の知らせ。
    void DrawStatusRow(EditorContext& ctx);
    void DrawConflictBar();
    void DrawEmptyState(EditorContext& ctx);
    void DrawProperties(EditorContext& ctx);

    std::unique_ptr<fluideditor::State> m_state;
    renderer::ResourceManager* m_resources = nullptr;
    /// OnShutdown は ctx を受け取らないが、Baked タブの GPU 資源を返すのに要る。毎フレーム覚え直す。
    EditorContext* m_context = nullptr;

    float m_outlinerWidth = 230.0f;
    float m_propertiesWidth = 340.0f;
    float m_timelineHeight = 200.0f;
    int m_newPresetIndex = 0;
    /// 焼き上がったフレームに Baked タブを前へ出す。
    bool m_selectBakedTab = false;
    /// 「自動保存しました」を出しておく残り時間 [秒]。
    float m_autoSaveNotice = 0.0f;
    /// Asset Browser の選択に追従するか (EditorSettings::fluidEditorFollowSelection が保存先)。
    bool m_followSelection = true;
    /// 追従したかったが未保存の変更で止めた相手。空でなければツールバーに «切り替える» を出す。
    std::string m_followBlockedPath;
    /// 最近開いた .fluid (新しい順・絶対パス)。保存先は EditorSettings::recentFluids。
    std::vector<std::string> m_recentFluids;

    std::uint32_t m_bakeJob = 0;
    std::uint32_t m_effectJob = 0;
    int m_effectPreset = 0;
    std::string m_effectDirectory;
    std::string m_effectError;
    bool m_bakeMakesVfx = false;
    std::string m_bakeVfxPath;
    /// AssetDirtyRegistry (終了時の一括保存) に載せている .fluid。空なら載せていない。
    std::string m_registeredDirtyPath;
};

} // namespace fbzz::editor
