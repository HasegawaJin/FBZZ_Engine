// FBZZ Engine
// VFXEditorPanel.hpp | fbzz::editor
// VFX Editor の合成ルート View。DockSpace ホスト・メニュー・ツールバー・各 View の配線を担う。
// WHY: グラフ、Inspector、決定論プレビューを OS の独立ウィンドウへまとめることで、
//      メイン Scene View のレイアウトと分離し、AI capture も専用 RT だけを対象にできる。
// NOTE: 以前はこのクラスが編集状態・描画・保存を全て抱えた 5000 行のファイルだった。
//       現在は「セッションを 1 つ持ち、View を並べ、ウィンドウの都合だけを見る」役に限定する。
//       状態は VFXEditorSession、描画は各 View、保存は VFXGraphDocument が持つ。
#pragma once

#include <Editor/VFXEditor/Application/VFXEditorSession.hpp>
#include <Editor/VFXEditor/Services/VFXRecipeLibrary.hpp>
#include <Editor/VFXEditor/Services/VFXTemplateCatalog.hpp>
#include <Editor/VFXEditor/Services/VFXTemplateThumbnailBaker.hpp>
#include <Editor/VFXEditor/Views/VFXGraphCanvas.hpp>
#include <Editor/VFXEditor/Views/VFXGraphInspector.hpp>
#include <Editor/VFXEditor/Views/VFXPreviewView.hpp>
#include <Editor/VFXEditor/Views/VFXTimelineView.hpp>
#include <Editor/Panels/EditorToolPanel.hpp>
#include <Engine/Scene/Entity.hpp>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fbzz::scene { struct ParticleEmitter; }

namespace fbzz::editor {

class VFXEditorPanel final : public EditorToolPanel {
public:
    const char* GetWindowName() const override { return "VFX Editor"; }
    const char* GetEditorType() const override { return "VFX Editor"; }
    bool GetDefaultVisibility() const override { return false; }
    void OnInit(EditorContext& ctx) override;
    void OnShutdown() override;

    // ホストアプリ (EditorApp / VFXEditorApp) から編集セッションへ触るための入口。
    // WHY: プレビュー RT の割り当てや環境設定の読み出しはアプリ側の責務だが、
    //      Panel の内部構造まで開く必要はない。Session だけを公開面にする。
    [[nodiscard]] VFXEditorSession& Session() { return m_session; }
    [[nodiscard]] const VFXEditorSession& Session() const { return m_session; }

    // 背面・最小化・画面外へ移動した独立viewportをEditor側から確実に復帰させる。
    void RequestFocusAndReveal(bool resetPlacement = false);
    // 独立ApplicationではPanelをメインOSウィンドウ全面に固定し、secondary viewport化を無効にする。
    void SetStandaloneApplicationMode(bool enabled) { m_standaloneApplicationMode = enabled; }
    // 独立AppのOS D&Dを、Graphを開く操作またはPreview Emitter生成へ振り分ける。
    void OpenDroppedAsset(EditorContext& ctx, const std::string& path);

    [[nodiscard]] bool HasUnsavedChanges() const { return m_session.document.dirty.IsDirty(); }
    [[nodiscard]] bool SaveCurrentGraph();
    [[nodiscard]] bool ReloadCurrentGraphFromDisk(const std::string& changedPath = {});

    [[nodiscard]] bool WantsPreviewRender() const { return visible && m_session.preview.renderRequested; }
    [[nodiscard]] bool UsesIsolatedPreviewWorld() const { return true; }
    [[nodiscard]] bool IsPreviewHovered() const { return m_session.preview.hovered; }
    // ループ継ぎ目の確認 (t=0 と t=duration を並べて描く) を要求しているか。
    [[nodiscard]] bool WantsLoopSeamPreview() const { return m_session.preview.showLoopSeam; }

protected:
    void OnRenderContent(EditorContext& ctx) override;
    void OnBeforeBegin(EditorContext& ctx) override;
    ImGuiWindowFlags GetWindowFlags() const override
    {
        // このWindowは操作パネルではなくDockSpaceホストなので、子の横幅でスクロールさせない。
        // WHY: Toolbarがウィンドウ幅を超えるとScrollXが残り、DockSpaceの左右端まで画面外へずれるため。
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_MenuBar
            | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (m_standaloneApplicationMode)
            flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove
                | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
        return flags;
    }
    bool CanClose() const override { return !m_standaloneApplicationMode; }

private:
    // ── 単体 Emitter モード (Preview World のエフェクト階層) ──
    void DrawHierarchy(EditorContext& ctx);
    void DrawEmitterNode(EditorContext& ctx, scene::EntityID id, int depth,
                         std::vector<scene::EntityID>& visited);

    // ── Graph モード ──
    void DrawGraphEditor(EditorContext& ctx);
    // 空間の親子 (VFXGraphNode::parentNodeId) を木として描き、D&D で組み替える。
    // WHY: Canvas は「いつ動くか」(link) を編集する面で、線が増えるほど親子関係は読めなくなる。
    //      位置の入れ子は木で見るのが最短で、Unity の Hierarchy と同じ操作感にもなる。
    //      同じデータの 2 つ目のビューであって、別のデータを持つわけではない。
    void DrawGraphHierarchy(EditorContext& ctx);
    // 1 ノードとその子孫を再帰描画する。activeNodeIds は実行中ノード (ライブ表示用)。
    void DrawGraphHierarchyNode(EditorContext& ctx, int nodeId,
                                const std::unordered_set<int>& activeNodeIds,
                                std::vector<int>& visited,
                                int& pendingParentChild, int& pendingParentTarget);
    // Graph モードの画面構成そのもの (検証バナー → メニュー → ツールバー → 各 View)。
    void DrawGraphLayout(EditorContext& context);
    void DrawGraphMenuBar(EditorContext& ctx);
    void DrawGraphToolbar(EditorContext& ctx);
    void DrawGraphStatusBar();
    void DrawGraphTemplateMenu(EditorContext& ctx);
    // カタログ 1 件ぶんのプレビュー (サムネイル・内訳・budget・不足素材・層)。
    // メニューとブラウザの両方から呼ぶので、表示の正本をここへ 1 つだけ置く。
    void DrawTemplateEntryPreview(EditorContext& ctx, const GraphTemplateEntry& entry,
                                  bool compact);
    // Replace の確認ダイアログと「現在のGraphをTemplateとして保存」ダイアログ。
    // WHY: メニュー階層の内側では modal を開けないため、要求だけフラグへ積んで
    //      ウィンドウ直下のこの関数で描画する。
    void DrawGraphTemplateDialogs(EditorContext& ctx);
    // 取り込み範囲・接続先・Variant を決めてから Merge / Sub Graph を実行するダイアログ。
    void DrawTemplateApplyDialog(EditorContext& ctx);
    // 取り込み結果 (改名・budget 引き上げ・不足素材) の報告。
    void DrawTemplateMergeReport();
    // 目的と規模を選んで骨格を生成する Recipe ウィザード。
    void DrawRecipeWizardDialog(EditorContext& ctx);
    // Template を適用する共通経路 (ダイアログ・メニューの両方がここを通る)。
    void ApplyTemplateWithOptions(EditorContext& ctx, const GraphTemplateEntry& entry,
                                  TemplateApplyMode mode, bool saveImmediately);
    // コンパイルエラーの先頭を制作画面から常に見える赤いバナーとして描く。
    void DrawShaderCompileErrorBanner();
    // Debugメニューへ診断件数・詳細表示・クリア操作を追加する。
    void DrawShaderCompileDebugMenuItems();
    // path/entry point/target/コンパイラ本文を省略せず確認できる詳細Windowを描く。
    void DrawShaderCompileDiagnosticsWindow();

    // VFX Editor内の各制作パネルを受け持つDockSpaceを描画する。
    // WHY: 親EditorのDockSpaceとはIDスコープを分け、VFX固有の配置を独立して保存するため。
    void DrawDockWorkspace();
    void BuildDefaultDockLayout(ImGuiID dockId);

    // ── 所有物 (宣言順 = 構築順。View は Session と互いを参照するため順序が意味を持つ) ──
    VFXEditorSession m_session;
    VFXGraphCanvas   m_canvas{ m_session };
    VFXPreviewView   m_previewView{ m_session, m_canvas };
    VFXGraphInspector m_inspector{ m_session, m_canvas, m_previewView };
    VFXTimelineView  m_timeline{ m_session };

    // ── ウィンドウ / ホスト都合の状態 ──
    bool m_standaloneApplicationMode = false;
    bool m_focusWindowRequested = false;
    bool m_resetWindowPlacementRequested = false;
    bool m_resetDockLayoutRequested = false;
    // Hierarchy が既定レイアウトに載っているかを起動後 1 度だけ確かめるためのフラグ。
    bool m_hierarchyDockChecked = false;
    bool m_graphWarningsExpanded = false;
    bool m_shaderDiagnosticsOpen = false;

    // ── Template ダイアログの状態 ──
    GraphTemplateEntry m_pendingTemplate;
    TemplateApplyMode m_pendingMode = TemplateApplyMode::Merge;
    bool m_openTemplateConfirm = false;
    bool m_openTemplateApply = false;
    bool m_templateSaveImmediately = false;
    bool m_openSaveTemplateDialog = false;
    bool m_openMergeReport = false;
    std::string m_saveTemplateName;
    std::string m_saveTemplateCategory;
    std::string m_saveTemplateDescription;
    std::string m_saveTemplateTags;
    // カタログ検索。ノードサーチャーと同じく大小無視の部分一致。
    std::string m_templateFilter;
    // 取り込み指定。ダイアログが編集し、適用時にそのまま Session へ渡す。
    vfx::TemplateMergeOptions m_mergeOptions;
    // groupFilter のチェック状態 (層 id -> 取り込むか)。
    std::vector<std::pair<int, bool>> m_layerSelection;
    int m_pendingVariantIndex = 0;

    // ── Recipe ウィザードの状態 ──
    bool m_openRecipeWizard = false;
    int m_recipeIndex = 0;
    int m_recipeScaleIndex = 1; // 0=S / 1=M / 2=L
    bool m_recipeLoop = false;
    bool m_recipeLight = true;
    bool m_recipeShake = false;
    bool m_recipeReplaceCurrent = true;
    std::string m_recipeStatus;
    // ロール -> 選んだテクスチャのプロジェクト相対パス。
    std::vector<std::pair<std::string, std::string>> m_recipeRoleTextures;

    // ── サムネイル ──
    // 画像自体の解決とキャッシュは widgets::ResolveAssetThumbnail が持つ
    // (ノードのサムネイルと同じ経路。別キャッシュを作るとリセット時の破棄が二重になる)。
    VFXTemplateThumbnailBaker m_thumbnailBaker;
};

} // namespace fbzz::editor
