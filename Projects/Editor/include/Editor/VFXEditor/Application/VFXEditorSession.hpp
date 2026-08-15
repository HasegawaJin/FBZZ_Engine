// FBZZ Engine
// VFXEditorSession.hpp | fbzz::editor
// VFX 編集セッション: ドキュメント・サービス・View 間の共有状態をまとめる編集の中心
// WHY: Canvas / Inspector / Timeline / Preview は「同じグラフの、同じ選択、同じ再生位置」を
//      見ていなければ意味がない。その共有点を Panel の private メンバーに置くと、
//      View を増やすたびに Panel が肥大化し、View 単体でのテストも不可能になる。
//      Session を明示的な合流点にすることで、各 View は Session への参照だけを持てばよくなる。
// NOTE: Session は View を知らない。View 側の一時状態を捨てさせる必要がある操作
//       (グラフ差し替え・ノード選択予約) だけを、コールバックとして受け取る。
#pragma once

#include <Editor/VFXEditor/Document/VFXGraphDocument.hpp>
#include <Editor/VFXEditor/Document/VFXGraphOps.hpp>
#include <Editor/VFXEditor/Services/VFXPreviewController.hpp>
#include <Editor/VFXEditor/Services/VFXSequenceExporter.hpp>
#include <Editor/VFXEditor/Services/VFXTemplateCatalog.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fbzz::editor {

struct EditorContext;

class VFXEditorSession {
public:
    // ── 所有物 ──
    VFXGraphDocument document;
    VFXPreviewController preview;
    VFXTemplateCatalog templates;
    VFXSequenceExporter sequenceExport;

    // ── View 間で共有する選択状態 ──
    int selectedNodeId = -1;
    int selectedLinkIndex = -1;
    int selectedGroupId = -1;
    // Canvas へ「選択中ノードを画面内へ入れてほしい」と伝える 1 回限りの要求。
    bool focusSelectionRequested = false;
    bool frameAllRequested = false;
    // ノード座標をアセット値から ImNodes へ流し込み直す必要があるか。
    bool positionsPending = false;
    // Inspector のドラッグ中など、Undo をまとめたい編集が進行中か。
    bool graphEditInProgress = false;

    // ── モード ──
    // .vfx 選択中は Graph エディタ、そうでなければ単体 Emitter 編集。
    bool graphMode = false;
    bool previousGraphMode = false;
    // Asset Browser から開き直したい .vfx。次フレームの先頭で選択へ反映する。
    std::string requestedAssetPath;

    // ── SubGraph のドリルダウン ──
    // 「今開いているグラフへ至るまでに通ってきた親グラフ」のパン屑。
    // WHY: SubGraph ノードは中身が一切見えない箱で、編集するには Asset Browser で
    //      別ファイルを探して開き直すしかなかった。往復のたびに文脈 (どの親から来たか) が
    //      失われるため、SubGraph が事実上使われないままになっていた。
    //      入るときに親を積み、戻るときに取り出す。スタックなので多段の入れ子も辿れる。
    // NOTE: 保持するのはパスだけ。親グラフの未保存変更は EnterSubGraph が入る前に保存させる
    //       (中へ入ってから親が dirty のままだと、どちらを正とするか決められなくなる)。
    std::vector<std::string> subGraphBreadcrumb;
    // 直近の requestedAssetPath がドリルダウン由来か。false なら Asset Browser 等からの
    // 無関係な .vfx なので、Panel がパン屑を捨てる (別の階層構造へ迷い込ませない)。
    bool subGraphNavigationPending = false;
    // 独立 VFX Editor が所有する Asset Browser の表示フラグ (所有はしない)。
    bool* assetBrowserVisible = nullptr;

    // ── 表示設定 (vfx_editor_settings.toml へ永続化する) ──
    bool showGraphGrid = true;
    bool showMiniMap = true;
    bool showTimeline = true;
    float graphZoom = 1.0f;
    bool timelineSnap = true;
    float timelineSnapStep = 0.05f;
    // 未保存グラフをプレビューへ流し込む (保存しないと反映されない状態をなくす)。
    bool liveEditEnabled = true;
    // 追加したノードを選択中ノードの下流へ自動接続する (孤立ノードの量産を防ぐ)。
    bool autoConnectNewNodes = true;

    // ── ライブ反映 / 保存待ちレジストリの重複抑止 ──
    std::uint64_t lastPushedGraphRevision = 0;
    std::uint64_t lastRegisteredDirtyRevision = 0;

    std::string preferencesPath;

    // ── View へのフック (Panel が OnInit で結線する) ──
    // グラフ丸ごと差し替え後に、Canvas の一時状態を捨てさせる。
    // resetPanning=true でパン位置も原点へ戻す (別グラフを開いたとき)。
    std::function<void(bool resetPanning)> onGraphReplaced;
    // 次フレームに描画されてから選択させたいノード id 群。
    std::function<void(std::vector<int>)> onSelectNodes;

    void LoadEditorPreferences(const EditorContext& ctx);
    void SaveEditorPreferences() const;

    // NOTE: 読み込み失敗は document.error に残り、エラーバナーが毎フレーム表示する。
    //       呼び出し側が戻り値を無視しても状態は正しく伝わるので nodiscard にしない。
    bool LoadGraph(const std::string& path);
    [[nodiscard]] bool SaveGraph();

    void PushUndo();
    void PushUndo(const asset::VFXGraphAsset& before);
    void Undo();
    void Redo();

    // Solo の切り替え。値の変更ではないので dirty は触らず、プレビューの再送出だけ促す。
    void ToggleSolo(int nodeId);

    // SubGraph ノードの中へ入る。現在のグラフをパン屑へ積んでから subGraphPath を開く。
    // 未保存の変更があれば保存してから移動する (保存に失敗したら移動しない)。
    // 戻り値 false = 移動しなかった (保存失敗 / パスが空 / 読み込み失敗)。
    bool EnterSubGraph(const std::string& subGraphPath);

    // パン屑を 1 段戻る (depth 指定でそこまで一気に戻る。-1 で 1 段)。
    // 戻り値 false = 戻る先が無い / 保存に失敗した。
    bool LeaveSubGraph(int targetDepth = -1);

    // 落とされたアセットから Preview World へエミッターを作る。
    // .vfx なら生成せず、Graph エディタで開く要求として扱う。
    void CreatePreviewEmitterFromAsset(EditorContext& ctx, const std::string& sourcePath);

    // Template 適用の結果。ダイアログが「何が起きたか」を出すために保持する。
    // WHY: 改名されたパラメーターも引き上げられた budget も不足素材も、
    //      出さなければ「取り込んだのに何か違う」としか判らない。
    vfx::TemplateMergeReport lastMergeReport;
    bool hasMergeReport = false;

    // Replace: 現在のGraphを丸ごと差し替える。
    // Merge:   現在のGraphへノード群を追記する (options で範囲・接続先を指定)。
    // SubGraph: Template を複製せず Sub Graph ノードとして参照する。
    //           WHY: Merge はコピーなので、Template を直しても取り込み済みのグラフへは
    //                伝播しない。「更新が伝わる基底」が要る場面はこちらを使う。
    [[nodiscard]] bool ApplyTemplate(EditorContext& ctx, const GraphTemplateEntry& entry,
                                     TemplateApplyMode mode,
                                     const vfx::TemplateMergeOptions& options,
                                     bool saveImmediately);

private:
    // Template を複製せず Sub Graph ノード 1 個として参照する。
    // 呼び出し元は ApplyTemplate のみ (Undo の push は呼び出し側が済ませている)。
    [[nodiscard]] bool LinkTemplateAsSubGraph(const GraphTemplateEntry& entry,
                                              const vfx::TemplateMergeOptions& options,
                                              std::string* outError);
    // 現在のグラフの Variant Set を公開パラメーターの既定値へ焼き込む。
    void ApplyVariantAsDefaults(const std::string& variantName);
};

} // namespace fbzz::editor
