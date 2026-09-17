/// @file    GraphLayoutAlgo.cpp
/// @brief   深さベース列配置の実装。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Editor/GraphEditor/GraphLayoutAlgo.hpp>

#include <algorithm>
#include <queue>
#include <unordered_set>

namespace fbzz::editor {

namespace {

/// 到達不能ノードを置く列のオフセット (最大深さ + この値)。
constexpr int UNREACHABLE_COLUMN_OFFSET = 1;

} // namespace

std::unordered_map<int, ImVec2> ComputeGraphLayout(
    std::span<const int> nodeIds,
    std::span<const GraphLayoutEdge> edges,
    std::span<const int> rootIds,
    const GraphLayoutOptions& options)
{
    std::unordered_map<int, ImVec2> positions;
    if (nodeIds.empty()) return positions;

    std::unordered_set<int> known(nodeIds.begin(), nodeIds.end());

    /// @note 隣接リスト (存在しないノードを指す辺は捨てる)
    std::unordered_map<int, std::vector<int>> children;
    for (const GraphLayoutEdge& edge : edges) {
        if (known.count(edge.from) == 0 || known.count(edge.to) == 0) continue;
        children[edge.from].push_back(edge.to);
    }

    /// @name 深さの決定
    /// @note 幅優先で回し、より深い経路が見つかったら更新して再伝播する
    ///       (最短経路を使うと後段のノードが前段より左に来て矢印が逆流して見えるため)。
    /// @note 深さは単調増加でしか更新せず、上限 (ノード数) を超えたら打ち切る。循環があっても有限回で収束する。
    std::unordered_map<int, int> depth;
    std::queue<int> pending;

    for (const int root : rootIds) {
        if (known.count(root) == 0) continue;
        depth[root] = 0;
        pending.push(root);
    }

    const int maxDepthBound = static_cast<int>(nodeIds.size());
    while (!pending.empty()) {
        const int current = pending.front();
        pending.pop();

        const int currentDepth = depth[current];
        /// @note 循環の保険
        if (currentDepth >= maxDepthBound) continue;

        const auto it = children.find(current);
        if (it == children.end()) continue;

        for (const int child : it->second) {
            const auto existing = depth.find(child);
            if (existing == depth.end() || existing->second < currentDepth + 1) {
                depth[child] = currentDepth + 1;
                pending.push(child);
            }
        }
    }

    int maxDepth = 0;
    for (const auto& [id, value] : depth) maxDepth = std::max(maxDepth, value);

    /// @name 列へ振り分け
    /// @note 到達不能ノードは最終列の 1 つ右へまとめる。
    const int unreachableColumn = maxDepth + UNREACHABLE_COLUMN_OFFSET;

    std::unordered_map<int, std::vector<int>> columns;
    for (const int id : nodeIds) {
        const auto it = depth.find(id);
        const int column = it != depth.end() ? it->second : unreachableColumn;
        columns[column].push_back(id);
    }

    /// @name 座標の割り当て
    /// @note 列内の並びは nodeIds の順序を保つ (入れ替わるとユーザーが覚えた位置関係が壊れて見えるため)。
    std::size_t maxRows = 0;
    for (const auto& [column, ids] : columns) maxRows = std::max(maxRows, ids.size());

    for (const auto& [column, ids] : columns) {
        const float x = options.originX + options.columnStep * static_cast<float>(column);

        /// @note 列ごとに縦センタリングする場合のオフセット。
        float yOffset = 0.0f;
        if (options.centerColumns && maxRows > ids.size()) {
            yOffset = (static_cast<float>(maxRows - ids.size()) * options.rowStep) * 0.5f;
        }

        for (std::size_t row = 0; row < ids.size(); ++row) {
            positions[ids[row]] = {
                x,
                options.originY + yOffset + options.rowStep * static_cast<float>(row)
            };
        }
    }

    return positions;
}

std::unordered_map<int, ImVec2> ComputeGraphLayout(
    std::span<const int> nodeIds,
    std::span<const GraphLayoutEdge> edges,
    const GraphLayoutOptions& options)
{
    /// @note 入次数 0 のノードを根とみなす。
    std::unordered_set<int> known(nodeIds.begin(), nodeIds.end());
    std::unordered_set<int> hasIncoming;
    for (const GraphLayoutEdge& edge : edges) {
        if (known.count(edge.from) == 0 || known.count(edge.to) == 0) continue;
        hasIncoming.insert(edge.to);
    }

    std::vector<int> roots;
    for (const int id : nodeIds) {
        if (hasIncoming.count(id) == 0) roots.push_back(id);
    }

    /// @note 全体が循環していて入次数 0 が無い場合、先頭を根にして最低限並べる (空で返すと壊れているように見えるため)。
    if (roots.empty() && !nodeIds.empty()) roots.push_back(nodeIds[0]);

    return ComputeGraphLayout(nodeIds, edges, roots, options);
}

} // namespace fbzz::editor
