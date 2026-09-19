/// @file    GraphLayoutAlgo.hpp
/// @brief   有向グラフの自動整列 (深さベースの列配置)。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note VFXGraphCanvas.cpp:399-453 と AnimationGraphPanel.cpp:3505-3527 が別々に整列を実装しており、前者は深さベースで実用的、後者は木構造を反映しない「4 列の単純グリッド」で結果の質が違っていた。ノード位置の保存先はツールごとに違う (VFX は editorX/editorY、Animation は EditorContext::graphLayouts) ため、計算だけ行い書き込みは呼び出し側に任せる。
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
    float columnStep = 300.0f;   ///< 深さ 1 段ぶんの横間隔
    float rowStep    = 190.0f;   ///< 同一列内の縦間隔
    float originX    = 80.0f;
    float originY    = 80.0f;

    /// 列ごとに縦方向のセンタリングを行うか。
    /// @note 列ごとの要素数が違うとき、上詰めだと全体が斜めに見えて読みづらいため既定で有効。中心を揃えると木の対称性が見える。
    bool centerColumns = true;
};

/// nodeIds の各ノードへ論理座標を割り当てる。
///
/// 列 = rootIds からの**最長**経路長。
/// @note DAG で複数経路があるとき最短だと後段のノードが前段より左に来て矢印が逆流して見えるため、最長を使い必ず左→右にする。
///
/// rootIds から到達できないノードは最終列の 1 つ右へまとめて隔離する (エディタで枝を一時的に外しても整列できるように捨てない)。
/// 循環を含む入力でも停止する (訪問済みノードは再訪しない)。
[[nodiscard]] std::unordered_map<int, ImVec2> ComputeGraphLayout(
    std::span<const int> nodeIds,
    std::span<const GraphLayoutEdge> edges,
    std::span<const int> rootIds,
    const GraphLayoutOptions& options = {});

/// rootIds を省略した版。入次数 0 のノードを根とみなす。
/// 入次数 0 が 1 つも無い (全体が循環している) 場合は nodeIds の先頭を根にする。
[[nodiscard]] std::unordered_map<int, ImVec2> ComputeGraphLayout(
    std::span<const int> nodeIds,
    std::span<const GraphLayoutEdge> edges,
    const GraphLayoutOptions& options = {});

} // namespace fbzz::editor
