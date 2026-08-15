// FBZZ Engine
// VFXGraphDocument.cpp | fbzz::editor
// 編集中の .vfx ドキュメント実装
#include <Editor/VFXEditor/Document/VFXGraphDocument.hpp>

#include <Editor/EditorContext.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <Editor/VFXEditor/Document/VFXGraphOps.hpp>

namespace fbzz::editor {

// 共有ヘルパー (ノード色・アイコン・パラメーター編集など) は fbzz::editor::vfx に
// 隔離してある。Editor 全体の名前空間を汚さないための境界。
using namespace vfx;

asset::VFXGraphAsset VFXGraphDocument::MakeSoloFilteredGraph() const
{
    asset::VFXGraphAsset filtered = graph;
    if (soloNodeId <= 0) return filtered;
    // Entry を残さないとスケジュールが組めないので、Entry と対象ノード以外を無効化する。
    // リンクと時間は保ったままなので、Solo 中も開始タイミングは本番と同じになる。
    for (auto& node : filtered.nodes)
        if (node.type != asset::VFXNodeType::Entry && node.id != soloNodeId)
            node.enabled = false;
    return filtered;
}



void VFXGraphDocument::RefreshUnreachableNodes()
{
    unreachableNodes.clear();
    const auto entry = std::find_if(graph.nodes.begin(), graph.nodes.end(),
        [](const asset::VFXGraphNode& node) { return node.type == asset::VFXNodeType::Entry; });
    if (entry == graph.nodes.end()) {
        // Entry が無ければ全ノードが起動しない。全部を警告対象として扱う。
        for (const auto& node : graph.nodes) unreachableNodes.push_back(node.id);
        return;
    }
    // Entry から前方向リンクだけを辿る素直な BFS。ノード数は多くても数十のため線形探索で足りる。
    std::vector<int> reachable{ entry->id };
    for (std::size_t head = 0; head < reachable.size(); ++head) {
        const int current = reachable[head];
        for (const auto& link : graph.links) {
            if (link.fromNode != current) continue;
            if (std::find(reachable.begin(), reachable.end(), link.toNode) == reachable.end())
                reachable.push_back(link.toNode);
        }
    }
    for (const auto& node : graph.nodes)
        if (std::find(reachable.begin(), reachable.end(), node.id) == reachable.end())
            unreachableNodes.push_back(node.id);
}


bool VFXGraphDocument::IsNodeUnreachable(int id) const
{
    return std::find(unreachableNodes.begin(), unreachableNodes.end(), id)
        != unreachableNodes.end();
}

} // namespace fbzz::editor
