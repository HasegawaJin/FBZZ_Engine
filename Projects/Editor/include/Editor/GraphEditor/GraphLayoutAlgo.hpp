/// @file    GraphLayoutAlgo.hpp
/// @brief   有向グラフの自動整列 (深さベースの列配置)。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// WHY 共通化するか:
/// VFXGraphCanvas.cpp:399-453 と AnimationGraphPanel.cpp:3505-3527 が
/// 別々に整列を実装している。前者は深さベースで実用的、後者は
/// 「4 列の単純グリッド」で木構造を反映しない。同じ操作なのに
/// ツールによって結果の質が違う状態だった。
///
/// WHY 座標だけを返すか:
/// ノード位置の保存先はツールごとに違う (VFX はノード自身の editorX/editorY、
/// Animation は EditorContext::graphLayouts)。計算だけして書き込みは任せる。
#pragma once
#include <imgui.h>

#include <span>
#include <unordered_map>
#include <vector>

namespace fbzz::editor {

struct GraphLayoutEdge {
    int from = 0;
    int to   = 0;
};

struct GraphLayoutOptions {
    float columnStep = 300.0f;   // 深さ 1 段ぶんの横間隔
    float rowStep    = 190.0f;   // 同一列内の縦間隔
    float originX    = 80.0f;
    float originY    = 80.0f;

    // 列ごとに縦方向のセンタリングを行うか。
    // WHY 既定で有効か: 列ごとの要素数が違うとき、上詰めだと全体が
    //     斜めに見えて構造が読みづらい。中心を揃えると木の対称性が見える。
    bool centerColumns = true;
};

// nodeIds の各ノードへ論理座標を割り当てる。
//
// 列 = rootIds からの**最長**経路長。
// WHY 最長か: DAG で複数の経路があるとき最短を使うと、後段のノードが
//     前段より左に来て矢印が逆流して見える。最長なら必ず左→右になる。
//
// rootIds から到達できないノードは最終列の 1 つ右へまとめて隔離する。
// WHY 捨てないか: エディタで枝を一時的に外して試すことがあり、
//     その状態でも整列を実行できないと不便。
//
// 循環を含む入力でも停止する (訪問済みノードは再訪しない)。
[[nodiscard]] std::unordered_map<int, ImVec2> ComputeGraphLayout(
    std::span<const int> nodeIds,
    std::span<const GraphLayoutEdge> edges,
    std::span<const int> rootIds,
    const GraphLayoutOptions& options = {});

// rootIds を省略した版。入次数 0 のノードを根とみなす。
// 入次数 0 が 1 つも無い (全体が循環している) 場合は nodeIds の先頭を根にする。
[[nodiscard]] std::unordered_map<int, ImVec2> ComputeGraphLayout(
    std::span<const int> nodeIds,
    std::span<const GraphLayoutEdge> edges,
    const GraphLayoutOptions& options = {});

} // namespace fbzz::editor
