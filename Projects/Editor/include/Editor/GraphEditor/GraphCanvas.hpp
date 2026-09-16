/// @file    GraphCanvas.hpp
/// @brief   ノードグラフ編集の共通キャンバス (ImNodes ラッパー)。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// 責務:
/// ImNodes コンテキストの寿命 / テーマ / カーソル基準ズーム / パン /
/// ノード・リンク・グループの描画 / 選択・移動・接続・削除の解釈 /
/// ミニマップ / グリッド / Frame All・Frame Selection / キーボード操作 /
/// ピン hover・コンテキストメニュー / 一時エラーハイライト / Asset D&D 受け
/// ノード・ピン・リンク・グループのビュー単位スタイル適用
///
/// 責務でないもの (ツール側に残す):
/// データモデル / リンクの妥当性検証 / Undo / Inspector / アセットの保存
///
/// WHY この分割か: Docs/design/graph-editor-framework.md を参照。
/// 要点は「3 つのグラフはデータモデルが根本的に違うので、共通の抽象モデルを
/// 作ると和集合になってどのツールにも使いにくい型ができる」。
///
/// ビルド:
/// imnodes は fbzz_editor が PUBLIC リンクする正式なライブラリターゲット。
/// `#include <imnodes.cpp>` で実装を .obj へ抱え込む必要はない (以前は
/// AnimationGraphPanel.cpp がそうしており、「他の .cpp は真似してはならない」という
/// 不変条件をこのコメントでしか守れなかった。今は二重定義をリンカが弾く)。
#pragma once
#include <Editor/GraphEditor/GraphInteraction.hpp>
#include <Editor/GraphEditor/GraphView.hpp>

#include <string>
#include <vector>

struct ImNodesContext;
struct ImNodesEditorContext;

namespace fbzz::editor {

class GraphCanvas {
public:
    struct Config {
        // ImGui の子ウィンドウ ID。パネル内に複数のキャンバスを置く場合は変える。
        const char* id = "##graph_canvas";

        float minZoom = 0.45f;
        float maxZoom = 1.80f;

        bool showMiniMap = true;
        bool showGrid    = true;

        // キャンバス全体の文字倍率。ノードごとの title/body/pinFontScale と乗算する。
        // WHY: ズームは座標・枠・リンクも拡大するが、文字倍率は可読性のために独立して
        //      調整したい。遠景でも文字だけを少し大きくする用途に使う。
        float fontScale = 1.0f;

        // 編集を許可するか。false なら移動・接続・削除・貼り付けを受け付けない。
        // WHY: Play 中はグラフをランタイム監視専用にし、Scene / Undo を書き換えない
        //      (AnimationGraphPanel.cpp:48-52 の既存規約を全ツールへ広げる)。
        bool editable = true;

        // ノード配置を毎フレーム view の position で上書きするか。
        // false にすると ImNodes 側のドラッグ結果が保持される。
        // WHY 既定を true にするか: ツールが唯一の真実 (アセット) を持ち、
        //     キャンバスは表示するだけ、という一方向の流れを既定にする。
        bool applyNodePositions = true;
    };

    GraphCanvas() = default;
    ~GraphCanvas();

    GraphCanvas(const GraphCanvas&)            = delete;
    GraphCanvas& operator=(const GraphCanvas&) = delete;

    // IPanel::OnInit / OnShutdown から呼ぶ。
    // WHY パネルごとに専有するか: ImNodes のコンテキストはパン・ズーム・選択を
    //     内部に持つため、複数のグラフエディタで共有すると状態が混ざる。
    //     既存の AnimationGraphPanel / VFXGraphCanvas も同じ方針。
    void CreateContexts();
    void DestroyContexts();
    [[nodiscard]] bool IsReady() const { return m_nodesContext != nullptr; }

    // 1 フレーム描画し、ユーザー操作の解釈結果を返す。
    // ImGui のウィンドウ内で呼ぶこと (Begin/End はこの関数の外側)。
    [[nodiscard]] GraphInteraction Draw(const GraphView& view, const Config& config);

    // ツールが検証に失敗したときに呼ぶ。指定ノードを一定秒だけ赤く光らせる。
    // WHY 一時表示か: モーダルやトーストだと操作の流れが切れる。
    //     原因のノード自体が光れば、視線を動かさずに理由が分かる。
    void ReportError(std::string message, std::vector<int> nodeIds);
    [[nodiscard]] const std::string& CurrentError() const { return m_errorMessage; }

    // 次フレームで選択させる。
    // WHY 遅延させるか: このフレームで追加したノードは ImNodes の内部プールにまだ
    //     存在せず、即座に SelectNode を呼んでも無視される。
    void RequestSelection(std::vector<int> nodeIds);
    // ノード・リンク選択を明示的に破棄する。別グラフへ切り替えるときに使う。
    void ClearSelection();

    // 全ノードが収まるようパンとズームを合わせる (次フレームで適用)。
    void RequestFrameAll();
    // 現在選択中のノードだけを画面へ収める (次フレームで適用)。
    // WHY 全体表示と分けるか: 大規模グラフでは、編集対象へ移動するたびに
    // 全体へズームアウトすると、操作対象が読めなくなる。
    void RequestFrameSelection();

    void  ResetView();
    [[nodiscard]] float  Zoom() const { return m_zoom; }
    void   SetZoom(float zoom);
    [[nodiscard]] ImVec2 Panning() const;
    void   SetPanning(ImVec2 panning);

    // 画面座標 → 論理座標。ツールが独自のドロップ処理を書くとき用。
    // Draw() の後に呼ぶこと (キャンバス原点が確定している必要がある)。
    [[nodiscard]] ImVec2 ScreenToLogical(ImVec2 screenPos) const;
    [[nodiscard]] ImVec2 LogicalToScreen(ImVec2 logicalPos) const;

private:
    // ノード下端の進捗帯とリンク上を進む点。EndNodeEditor の後にだけ呼ぶ。
    void DrawProgressOverlay(const GraphView& view);

    ImNodesContext*       m_nodesContext  = nullptr;
    ImNodesEditorContext* m_editorContext = nullptr;

    float  m_zoom = 1.0f;
    ImVec2 m_canvasOrigin = {};   // Draw() の冒頭で確定するキャンバス左上 (画面座標)
    ImVec2 m_canvasSize   = {};

    // 一時エラー表示
    std::string      m_errorMessage;
    std::vector<int> m_errorNodes;
    float            m_errorRemaining = 0.0f;

    // 次フレームへ持ち越す要求
    std::vector<int> m_pendingSelection;
    bool             m_frameAllRequested = false;
    bool             m_frameSelectionRequested = false;
    bool             m_selectAllRequested = false;
    bool             m_clearSelectionRequested = false;

    // 接続ドラッグ中に掴んでいるピン。していなければ 0。
    // WHY メンバーで持つか: ImNodes::IsLinkStarted は掴んだ瞬間の 1 フレームしか
    //     true を返さず、しかも EndNodeEditor の後でしか問い合わせられない。
    //     減光は次フレームの BeginNodeEditor 内で行うため、跨いで覚えておく必要がある。
    int m_draggingFromPin = 0;

    // ドラッグ境界の検出 (1 操作 = 1 Undo)
    bool m_dragging = false;
    bool m_groupDragging = false;
    bool m_groupResizing = false;

    // 前フレームの選択。変化検出に使う。
    std::vector<int> m_lastSelectedNodes;
    std::vector<int> m_lastSelectedLinks;
};

} // namespace fbzz::editor
