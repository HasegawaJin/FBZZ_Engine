// FBZZ Engine
// VFXGraphEditOps.cpp | fbzz::editor::vfxops
// VFX Graph の構造編集 (VFXGraphCanvas と AI Command Bus の共有実装)
#include <Editor/VFXEditor/Document/VFXGraphEditOps.hpp>

#include <algorithm>
#include <vector>

namespace fbzz::editor::vfxops {

int NextNodeId(const asset::VFXGraphAsset& graph)
{
    int nextId = 1;
    for (const auto& node : graph.nodes) nextId = (std::max)(nextId, node.id + 1);
    return nextId;
}

float DefaultNodeDuration(const asset::VFXGraphNode& node)
{
    if (node.type == asset::VFXNodeType::Delay)    return 0.25f;
    if (node.type == asset::VFXNodeType::Particle) return node.particle.duration;
    return 1.0f;
}

void PlaceNodeOnDefaultGrid(const asset::VFXGraphAsset& graph, asset::VFXGraphNode& node)
{
    const std::size_t index = graph.nodes.size();
    node.editorX = 260.0f + static_cast<float>((index % 3) * 220);
    node.editorY = 80.0f  + static_cast<float>((index / 3) * 170);
}

asset::VFXGraphNode MakeNode(const asset::VFXGraphAsset& graph,
                             asset::VFXNodeType type,
                             const std::string& name)
{
    asset::VFXGraphNode node;
    node.id   = NextNodeId(graph);
    node.type = type;
    node.name = name.empty() ? asset::VFXNodeTypeName(type) : name;
    node.duration = DefaultNodeDuration(node);
    PlaceNodeOnDefaultGrid(graph, node);
    return node;
}

bool AddLink(asset::VFXGraphAsset& graph,
             int fromNode,
             int toNode,
             asset::VFXLinkTrigger trigger,
             float delay,
             bool validateSchedule)
{
    if (fromNode <= 0 || toNode <= 0) return false;

    // Entry は「起点」なので入力リンクを持たない。
    const auto target = std::find_if(graph.nodes.begin(), graph.nodes.end(),
        [toNode](const asset::VFXGraphNode& node) { return node.id == toNode; });
    if (target == graph.nodes.end() || target->type == asset::VFXNodeType::Entry) return false;

    graph.links.push_back({ fromNode, toNode, trigger, delay });
    if (!validateSchedule) return true;

    // 循環などでスケジュールが組めなくなったら取り消す。
    std::vector<float> starts;
    float duration = 0.0f;
    if (asset::BuildVFXGraphSchedule(graph, starts, duration, nullptr)) return true;
    graph.links.pop_back();
    return false;
}

bool RemoveNode(asset::VFXGraphAsset& graph, int nodeId)
{
    const auto node = std::find_if(graph.nodes.begin(), graph.nodes.end(),
        [nodeId](const asset::VFXGraphNode& item) { return item.id == nodeId; });
    if (node == graph.nodes.end() || node->type == asset::VFXNodeType::Entry) return false;

    std::erase_if(graph.nodes, [nodeId](const asset::VFXGraphNode& item) {
        return item.id == nodeId;
    });
    std::erase_if(graph.links, [nodeId](const asset::VFXGraphLink& item) {
        return item.fromNode == nodeId || item.toNode == nodeId;
    });
    // 公開パラメーターの binding を残すと、存在しないノードを指したまま保存が通り、
    // 「パラメーターを動かしても何も変わらない」という形でしか現れない。
    std::erase_if(graph.bindings, [nodeId](const asset::VFXParamBinding& item) {
        return item.nodeId == nodeId;
    });
    // 親を失った子は解決できない parentNodeId を抱えたままになる。空間の入れ子が
    // 黙って外れるので、明示的に「親なし」へ戻す。
    for (auto& remaining : graph.nodes)
        if (remaining.parentNodeId == nodeId) remaining.parentNodeId = -1;
    return true;
}

} // namespace fbzz::editor::vfxops
