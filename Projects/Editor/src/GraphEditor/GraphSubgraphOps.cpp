/// @file    GraphSubgraphOps.cpp
/// @brief   部分グラフ抽出と手動整列の実装。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Editor/GraphEditor/GraphSubgraphOps.hpp>

#include <algorithm>
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::editor {

GraphExtractResult ExtractSubgraph(std::span<const int> selected,
                                   std::span<const GraphEdge> edges,
                                   int& nextId,
                                   GraphBoundaryPolicy boundaryPolicy)
{
    GraphExtractResult result;
    std::unordered_set<int> inside;
    // 重複した選択と 0 以下の id を落としてから採番する。
    // WHY: 選択の重複をそのまま採番すると、同じ元ノードに 2 つの新 id が付き、
    //      リンクの写しがどちらへ繋ぐか決まらなくなる。
    for (const int nodeId : selected) {
        if (nodeId <= 0 || !inside.insert(nodeId).second) continue;
        result.idMap[nodeId] = ++nextId;
    }

    for (std::size_t index = 0; index < edges.size(); ++index) {
        const GraphEdge& edge = edges[index];
        const auto from = result.idMap.find(edge.from);
        const auto to   = result.idMap.find(edge.to);
        const bool fromInside = from != result.idMap.end();
        const bool toInside   = to != result.idMap.end();
        if (fromInside && toInside) {
            result.edges.push_back({ from->second, to->second });
            result.edgeSourceIndices.push_back(index);
            continue;
        }
        // 片側だけが選択に含まれる = 境界。
        if (!fromInside && !toInside) continue; // どちらも外側。そもそも無関係
        if (boundaryPolicy == GraphBoundaryPolicy::KeepSource && !fromInside && toInside) {
            // from は元のノードのまま、to だけ新しい id へ繋ぎ替える。
            result.edges.push_back({ edge.from, to->second });
            result.edgeSourceIndices.push_back(index);
            continue;
        }
        ++result.droppedBoundaryEdges;
    }
    return result;
}


std::vector<int> CollectReachable(std::span<const int> roots, std::span<const GraphEdge> edges)
{
    std::unordered_map<int, std::vector<int>> adjacency;
    for (const GraphEdge& edge : edges) adjacency[edge.from].push_back(edge.to);

    std::unordered_set<int> visited;
    std::deque<int> queue;
    for (const int root : roots)
        if (root > 0 && visited.insert(root).second) queue.push_back(root);

    std::vector<int> result;
    while (!queue.empty()) {
        const int current = queue.front();
        queue.pop_front();
        result.push_back(current);
        const auto next = adjacency.find(current);
        if (next == adjacency.end()) continue;
        // visited へ入れてから積むので、循環していても停止する。
        for (const int child : next->second)
            if (visited.insert(child).second) queue.push_back(child);
    }
    return result;
}


namespace {

// 選択のうち、座標が判っているものだけを集める。
std::vector<std::pair<int, ImVec2>> CollectKnown(
    const std::unordered_map<int, ImVec2>& positions, std::span<const int> selected)
{
    std::vector<std::pair<int, ImVec2>> known;
    std::unordered_set<int> seen;
    for (const int nodeId : selected) {
        if (!seen.insert(nodeId).second) continue;
        const auto found = positions.find(nodeId);
        if (found != positions.end()) known.emplace_back(nodeId, found->second);
    }
    return known;
}

} // namespace


std::unordered_map<int, ImVec2> AlignNodes(const std::unordered_map<int, ImVec2>& positions,
                                           std::span<const int> selected, GraphAlignMode mode)
{
    std::unordered_map<int, ImVec2> changed;
    const std::vector<std::pair<int, ImVec2>> known = CollectKnown(positions, selected);
    if (known.size() < 2) return changed; // 1 個では揃える相手がいない

    float minX = known.front().second.x, maxX = minX;
    float minY = known.front().second.y, maxY = minY;
    for (const auto& [nodeId, position] : known) {
        minX = (std::min)(minX, position.x);
        maxX = (std::max)(maxX, position.x);
        minY = (std::min)(minY, position.y);
        maxY = (std::max)(maxY, position.y);
    }
    // 中央は「重心」ではなく「外接矩形の中心」。重心だと 1 個だけ離れたノードに
    // 引っ張られて、揃えたつもりの列が斜めのまま残る。
    const float centerX = (minX + maxX) * 0.5f;
    const float centerY = (minY + maxY) * 0.5f;

    for (const auto& [nodeId, position] : known) {
        ImVec2 target = position;
        switch (mode) {
        case GraphAlignMode::Left:             target.x = minX;    break;
        case GraphAlignMode::HorizontalCenter: target.x = centerX; break;
        case GraphAlignMode::Right:            target.x = maxX;    break;
        case GraphAlignMode::Top:              target.y = minY;    break;
        case GraphAlignMode::VerticalCenter:   target.y = centerY; break;
        case GraphAlignMode::Bottom:           target.y = maxY;    break;
        }
        if (target.x != position.x || target.y != position.y) changed[nodeId] = target;
    }
    return changed;
}


std::unordered_map<int, ImVec2> DistributeNodes(const std::unordered_map<int, ImVec2>& positions,
                                                std::span<const int> selected, bool horizontal)
{
    std::unordered_map<int, ImVec2> changed;
    std::vector<std::pair<int, ImVec2>> known = CollectKnown(positions, selected);
    if (known.size() < 3) return changed; // 両端しか無ければ等間隔にする余地が無い

    std::sort(known.begin(), known.end(), [horizontal](const auto& a, const auto& b) {
        return horizontal ? a.second.x < b.second.x : a.second.y < b.second.y;
    });
    const float first = horizontal ? known.front().second.x : known.front().second.y;
    const float last  = horizontal ? known.back().second.x  : known.back().second.y;
    const float step  = (last - first) / static_cast<float>(known.size() - 1);

    for (std::size_t index = 1; index + 1 < known.size(); ++index) {
        ImVec2 target = known[index].second;
        const float value = first + step * static_cast<float>(index);
        if (horizontal) target.x = value; else target.y = value;
        if (target.x != known[index].second.x || target.y != known[index].second.y)
            changed[known[index].first] = target;
    }
    return changed;
}

} // namespace fbzz::editor
