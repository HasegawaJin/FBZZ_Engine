// FBZZ Engine
// VFXGraphCanvas.hpp | fbzz::editor
// ノードグラフのキャンバス View (ImNodes 描画・ノード編集操作・グループ枠)
// WHY: キャンバスは VFX Editor で最も状態の多い面 (ドラッグ中のグループ、貼り付け基準座標、
//      リンク予約、改名中のノード…) を持つ。これらを Panel に置くと、Panel を読む人が
//      「どれが表示、どれが編集、どれが一時状態か」を判別できなくなる。
//      キャンバス固有の一時状態はこのクラスの外へ出さない、を境界の原則にする。
#pragma once

#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Editor/GraphEditor/GraphCanvas.hpp>
#include <Editor/GraphEditor/GraphView.hpp>
#include <imgui.h>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

struct ImNodesContext;
struct ImNodesEditorContext;

namespace fbzz::editor {

struct EditorContext;
class VFXEditorSession;

// 選択ノードの整列方向。Panel のメニューからも指定するため namespace スコープに置く。
enum class GraphAlign { Left, CenterX, Right, Top, CenterY, Bottom };

class VFXGraphCanvas {
public:
    explicit VFXGraphCanvas(VFXEditorSession& session) : m_session(session) {}

    // ── ライフサイクル (ImNodes コンテキストの所有者) ──
    void CreateContexts();
    void DestroyContexts();

    // ── Session からのフック ──
    // グラフ丸ごと差し替え後に一時状態を捨てる。resetPanning でパン位置も原点へ戻す。
    void OnGraphReplaced(bool resetPanning);
    // Duplicate/Paste/Merge 直後の新規ノードは ImNodes のプールへ未登録のため、その場では選択できない。
    // 次フレームに描画されプールへ入ってから選択を適用するための予約。
    void RequestSelection(std::vector<int> ids);

    // 直近の操作が拒否された理由 (Panel のエラーバナーが読む)。
    [[nodiscard]] const std::string& TransientError() const { return m_transientGraphError; }

    void Draw(EditorContext& ctx);
    void DrawAddNodeMenu();
    void DrawNodeSearchBar();
    // SubGraph の階層パン屑。1 段でも潜っているときだけ Canvas 上部へ出る。
    void DrawSubGraphBreadcrumb();

    // ── ノード編集 (Panel のメニュー / ショートカットからも呼ぶ) ──
    void AddNode(asset::VFXNodeType type);
    void AddNodeFromAsset(const std::string& sourcePath);
    void DeleteSelectedNode();
    void DeleteSelectedNodes();
    void DuplicateSelectedNodes();
    void CopySelectedNodes(bool cut);
    // クリップボードのノード群を貼り付ける。atMouse 時は右クリック位置を基準に相対配置する。
    void PasteNodes(bool atMouse);
    // 指定ノードに繋がる全リンクを切る (Unreal Blueprint の Break All Pin Links 相当)。
    void BreakNodeLinks(int nodeId);
    // グループ枠(注釈)を追加する。実行DAGには関与しない。
    void AddGroup();

    void AlignSelectedNodes(GraphAlign mode);
    // 選択ノードを X または Y 方向へ等間隔に配置する。
    void DistributeSelectedNodes(bool horizontal);
    // Entry からのリンク深さを列、同じ列内の順序を行にして全ノードを並べ直す。
    void AutoLayoutGraph();

    // 単一の選択 id ではなく ImNodes のマルチ選択全体を対象にする。
    // 選択が空なら Session の selectedNodeId へフォールバックする。
    [[nodiscard]] std::vector<int> SelectedNodeIds() const;

    // ── Panel のメニューから操作する検索 / ノード追加ポップアップの状態 ──
    // WHY: 「Find Node...」「Add Effect」はメニューバー側の項目だが、実体はキャンバスの
    //      オーバーレイ。フラグだけ公開して、描画と挙動はこのクラスに閉じたままにする。
    bool nodeSearchOpen = false;
    bool nodeSearchFocusRequested = false;
    std::string addNodeFilter;
    bool addNodeFilterFocus = false;

    // Panel が持つ Template メニューをキャンバスの右クリックメニューへ差し込むためのフック。
    // WHY: Template カタログの表示は Panel の責務だが、追加操作の導線としては
    //      キャンバス上の右クリックが最短。View 同士を直接結ばず、断片だけを受け取る。
    std::function<void(EditorContext&)> onDrawTemplateMenu;

private:
    // 枠内に中心があるノード id を集める。グループの選択・移動・フィットで共有する。
    // NOTE: ノードの実寸は ImNodes にしか無いため、Document ではなくここが持つ。
    [[nodiscard]] std::vector<int> NodesInsideGroup(const asset::VFXGraphGroup& group) const;
    [[nodiscard]] bool NodeMatchesSearch(const asset::VFXGraphNode& node) const;
    void JumpToNextSearchMatch(int direction);
    // 操作が拒否された理由と原因ノードを一定時間だけ表示する。
    // WHY: document.error は毎フレームの検証結果で上書き・消去されるため、
    //      「Graph自体は正しいが今の操作は失敗した」種類のメッセージは1フレームで消えていた。
    void SetTransientGraphError(std::string message, std::initializer_list<int> ids);
    // 共通GraphCanvasへ渡す毎フレームのビューを構築する。
    [[nodiscard]] GraphView BuildGenericView(EditorContext& ctx);
    // 共通キャンバスが返した意図をVFXアセットへ適用する。
    void ApplyGenericInteraction(EditorContext& ctx, const GraphInteraction& interaction);

    VFXEditorSession& m_session;

    // すべてのグラフで同じパン・ズーム・入力解釈を使うための共通キャンバス。
    // NOTE: ImNodes のコンテキストはこの GraphCanvas が所有する。以前は VFX 自前の
    //       ImNodes 描画経路と共通経路が両方あり、コンテキストも二重に持っていた。
    //       描画されないほうのコードは壊れても気付けないため、旧経路ごと削除した。
    GraphCanvas m_graphCanvas;
    std::vector<int> m_genericSelectedNodes;
    std::vector<int> m_genericSelectedLinks;

    // ノード検索 (Ctrl+F)。名前とノード種別名の両方を大小無視で部分一致させる。
    char m_nodeSearchBuffer[64] = {};
    // Enter で次の一致へ送るための巡回位置。ヒット集合が変わったら 0 へ戻す。
    int m_nodeSearchCursor = 0;

    // 右クリックAddで生成したノードをクリック位置へ置くための1回限りの座標(Grid space)。
    float m_pendingNodeSpawnGridX = 0.0f;
    float m_pendingNodeSpawnGridY = 0.0f;
    bool m_pendingNodeSpawnValid = false;
    // Canvas 上の最新マウス Grid 座標。ペースト基準位置に使う。
    float m_lastCanvasMouseGridX = 0.0f;
    float m_lastCanvasMouseGridY = 0.0f;

    // ピンからドラッグしたリンクを空きスペースで離したときの接続予約。
    // 出力ピン発なら m_pendingLinkFromNode、入力ピン発なら m_pendingLinkToNode に入る。
    int m_pendingLinkFromNode = -1;
    int m_pendingLinkToNode = -1;

    // 右クリック対象のノード / グループ枠と、インライン改名中のノード。
    int m_graphContextMenuNodeId = -1;
    int m_contextGroupId = -1;
    int m_renameNodeId = -1;
    bool m_renameFocusRequested = false;
    char m_genericRenameBuffer[128] = {};

    // 直近のリンク失敗などで赤表示する原因ノード・メッセージと、その残り表示秒数。
    std::vector<int> m_errorNodes;
    std::string m_transientGraphError;
    float m_errorNodeHighlightSeconds = 0.0f;
};

} // namespace fbzz::editor
