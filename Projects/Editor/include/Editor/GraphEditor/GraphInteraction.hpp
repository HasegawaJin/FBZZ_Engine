/// @file    GraphInteraction.hpp
/// @brief   キャンバスが 1 フレームで解釈したユーザー操作。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// WHY 「意図」だけを返し、キャンバスが実行しないか:
/// リンクの妥当性ルールはツール固有 (BehaviorTree は循環と親の重複、
/// VFX はトポロジカルソート可能性、Animation は遷移の重複)。
/// キャンバスへコールバックで検証を渡すと、呼び出しタイミングがキャンバスの都合になり、
/// Undo の境界と噛み合わせるのが難しくなる。
/// 「作られようとした」という事実だけを返せば、ツールが自分の順序で
/// 検証 → 適用 → Undo を組める。
#pragma once
#include <imgui.h>

#include <string>
#include <vector>

namespace fbzz::editor {

struct GraphNodeMove {
    int    nodeId   = 0;
    ImVec2 position = {};   // 論理座標 (ズーム倍率を戻した後)
};

struct GraphGroupMove {
    int    groupId = 0;
    ImVec2 delta   = {};     // 論理座標での今回フレームの移動量
};

struct GraphGroupResize {
    int    groupId = 0;
    ImVec2 min     = {};     // 論理座標での現在の左上
    ImVec2 max     = {};     // 論理座標での現在の右下
};

struct GraphInteraction {
    // ── リンク ────────────────────────────────────────────────────────────
    // ピンからピンへドラッグして接続しようとした。
    bool linkCreated    = false;
    int  createdFromPin = 0;
    int  createdToPin   = 0;
    // ピンの持ち主。GraphView から作った逆引き表で解決済み。
    // WHY ピン ID だけで返さないか: 以前はツール側が「ピン ID の偶奇」(Animation) と
    //     「出力ピン ID の総当り」(VFX) で別々に逆引きしていた。ピンはそもそも
    //     GraphView のノードの中にあるので、キャンバスが答えられる情報だった。
    //     解決できない (view に無いピン) 場合は 0。
    int  createdFromNode = 0;
    int  createdToNode   = 0;

    // ピンを外してノード本体へドロップした (Unity 相当の操作性)。
    // WHY 別に返すか: 接続先のピンが確定していないため、どのピンへ繋ぐかは
    //     ツールがノードの種別を見て決める必要がある。
    bool linkDroppedOnNode = false;
    int  droppedFromPin    = 0;
    int  droppedFromNode   = 0;
    int  droppedOnNode     = 0;

    // 接続ドラッグの継続中に掴んでいるピンと、その持ち主。していなければ 0。
    // WHY 返すか: 減光そのものはキャンバスが GraphView::linkDragFilter で行うが、
    //     ツール側も「接続中はコンテキストメニューを出さない」等の判断に使える。
    int draggingFromPin  = 0;
    int draggingFromNode = 0;

    std::vector<int> destroyedLinks;   // リンクをドラッグで切った

    // ── 選択 ──────────────────────────────────────────────────────────────
    bool selectionChanged = false;
    std::vector<int> selectedNodes;
    std::vector<int> selectedLinks;
    // ピンの hover は型・接続方向・ポート固有メニューをツール側で解釈するために返す。
    int hoveredPin = -1;
    int hoveredPinNode = 0;   // hoveredPin の持ち主 (解決できなければ 0)

    // ── 移動 ──────────────────────────────────────────────────────────────
    std::vector<GraphNodeMove> movedNodes;
    std::vector<GraphGroupMove> movedGroups;
    std::vector<GraphGroupResize> resizedGroups;

    // 1 操作 = 1 Undo にまとめるための境界通知。
    // WHY キャンバスが Undo を持たないか: スナップショット方式 (VFXEditorSession) と
    //     LambdaCommand 方式 (AnimationGraphPanel) のどちらもツールのデータ構造に
    //     密結合している。境界だけ教えれば、ツールは自分の方式で 1 回だけ積める。
    bool dragStarted = false;
    bool dragEnded   = false;

    // ── コマンド (キーボードショートカット) ──────────────────────────────
    bool deleteRequested    = false;   // Delete
    bool duplicateRequested = false;   // Ctrl+D
    bool copyRequested      = false;   // Ctrl+C
    bool cutRequested       = false;   // Ctrl+X
    bool pasteRequested     = false;   // Ctrl+V
    bool selectAllRequested = false;   // Ctrl+A
    bool clearSelectionRequested = false; // Escape
    bool frameAllRequested = false;    // Home
    bool frameSelectionRequested = false; // F

    // ── 空白の右クリック ──────────────────────────────────────────────────
    bool   contextMenuRequested = false;
    ImVec2 contextSpawnPosition = {};  // 論理座標へ変換済み

    // ── ノード上の右クリック ──────────────────────────────────────────────
    bool nodeContextMenuRequested = false;
    int  contextMenuNode          = -1;
    bool nodeDoubleClicked        = false;
    int  doubleClickedNode        = -1;
    bool pinContextMenuRequested = false;
    int  contextMenuPin          = -1;
    bool groupContextMenuRequested = false;
    int  contextMenuGroup          = -1;
    bool linkContextMenuRequested = false;
    int  contextMenuLink          = -1;

    // ── Asset Browser からの D&D ──────────────────────────────────────────
    bool        assetDropped = false;
    std::string droppedAssetPath;
    ImVec2      dropPosition = {};     // 論理座標へ変換済み

    int hoveredNode = -1;
    int hoveredLink = -1;
    int hoveredGroup = -1;
    bool groupClicked = false;
    int clickedGroup = -1;

    // キャンバス (ノードでもリンクでもミニマップでもない領域) がホバーされている。
    bool canvasHovered = false;
};

} // namespace fbzz::editor
