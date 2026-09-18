/// @file    BehaviorTreeOps.cpp
/// @brief   Behavior Tree の構造編集 (BehaviorTreePanel と AI Command Bus の共有実装)。
/// @author  Hasegawa Jin
/// @date    2026-08-22
#include <Editor/GraphEditor/BehaviorTreeOps.hpp>

#include <Editor/GraphEditor/GraphLayoutAlgo.hpp>
#include <Editor/GraphEditor/GraphSubgraphOps.hpp>

#include <algorithm>
#include <unordered_set>

namespace fbzz::editor::btops {

namespace {

/// 深さ 1 段ぶんの横間隔。ノード幅より僅かに広いだけだとリンクが隣のノードへ
/// 潜り込み、どの枝がどこへ伸びているのか読めなくなるため 100px 強の余白を持たせる。
/// 移行前は BehaviorTreePanel 側の定数と AI 側の直書き 300.0f に分かれていた。
constexpr float kNodeMinWidth  = 196.0f;
constexpr float kNodeColumnStep = kNodeMinWidth + 104.0f;   ///< 300: テンプレートの手置き座標と一致
/// ノード高さ (タイトル + 本文 4 行 + ピン 2 行) が収まる縦間隔。
constexpr float kNodeRowStep = 170.0f;

/// 親子関係を辺として取り出す。ExtractSubgraph / CollectReachable へ渡す形。
std::vector<GraphEdge> ParentEdges(const fbzz::ai::BehaviorTreeAsset& asset)
{
    std::vector<GraphEdge> edges;
    for (const auto& node : asset.nodes)
        if (node.parentId != 0) edges.push_back({ node.parentId, node.id });
    return edges;
}

int ChildCount(const fbzz::ai::BehaviorTreeAsset& asset, int parentId)
{
    return static_cast<int>(std::count_if(
        asset.nodes.begin(), asset.nodes.end(),
        [parentId](const fbzz::ai::BTNodeDef& node) { return node.parentId == parentId; }));
}

} // namespace

std::vector<const fbzz::ai::BTNodeDef*> ChildrenOf(const fbzz::ai::BehaviorTreeAsset& asset,
                                                   int parentId)
{
    std::vector<const fbzz::ai::BTNodeDef*> children;
    for (const auto& node : asset.nodes)
        if (node.parentId == parentId) children.push_back(&node);
    std::sort(children.begin(), children.end(),
        [](const fbzz::ai::BTNodeDef* a, const fbzz::ai::BTNodeDef* b) { return a->order < b->order; });
    return children;
}

bool IsDescendant(const fbzz::ai::BehaviorTreeAsset& asset, int ancestorId, int childId)
{
    int current = childId;
    /// @note ノード数を上限にすれば、既に壊れて循環しているデータでも止まる。
    for (std::size_t guard = 0; guard <= asset.nodes.size() && current != 0; ++guard) {
        if (current == ancestorId) return true;
        const fbzz::ai::BTNodeDef* node = asset.FindNode(current);
        if (node == nullptr) return false;
        current = node->parentId;
    }
    return false;
}

bool FindNodeType(std::string_view name, fbzz::ai::BTNodeType& out)
{
    for (int index = 0; index < static_cast<int>(fbzz::ai::BTNodeType::Count); ++index) {
        const auto candidate = static_cast<fbzz::ai::BTNodeType>(index);
        if (name == fbzz::ai::BTNodeTypeName(candidate)) {
            out = candidate;
            return true;
        }
    }
    return false;
}

std::string TryReparentNode(fbzz::ai::BehaviorTreeAsset& asset, int childId, int newParentId)
{
    fbzz::ai::BTNodeDef* child = asset.FindNode(childId);
    if (child == nullptr) return "ノードが見つかりません";
    if (childId == newParentId) return "自分自身を親にはできません";

    if (newParentId != 0) {
        const fbzz::ai::BTNodeDef* parent = asset.FindNode(newParentId);
        if (parent == nullptr) return "親ノードが見つかりません";
        /// @note 子孫を親にすると循環する。BT は木なので必ず弾く。
        if (IsDescendant(asset, childId, newParentId))
            return "自分の子孫を親にはできません (循環します)";

        const int maxChildren = fbzz::ai::BTNodeMaxChildren(parent->type);
        if (maxChildren == 0)
            return std::string(fbzz::ai::BTNodeTypeName(parent->type))
                 + " は葉ノードなので子を持てません";
        /// @note 既に自分がその親の子なら、付け替えても数は増えない。
        const bool alreadyChild = child->parentId == newParentId;
        if (maxChildren > 0 && !alreadyChild && ChildCount(asset, newParentId) >= maxChildren)
            return std::string(fbzz::ai::BTNodeTypeName(parent->type)) + " が持てる子は "
                 + std::to_string(maxChildren) + " 個までです";
    } else {
        /// @note 親なし = ルート。木にルートは 1 つだけ。
        for (const auto& node : asset.nodes)
            if (node.parentId == 0 && node.id != childId)
                return "ルートは 1 つだけです (既存のルートへ繋いでください)";
    }

    child->parentId = newParentId;
    /// @note 末尾へ追加する。優先度は order なので、後から Inspector か D&D で並べ替える。
    int nextOrder = 0;
    for (const auto& node : asset.nodes)
        if (node.parentId == newParentId && node.id != childId)
            nextOrder = (std::max)(nextOrder, node.order + 1);
    child->order = nextOrder;
    return {};
}

AddNodeResult AddNode(fbzz::ai::BehaviorTreeAsset& asset,
                      fbzz::ai::BTNodeType type,
                      const std::string&   name,
                      int                  parentId,
                      float                editorX,
                      float                editorY,
                      bool                 orphanOnReject)
{
    AddNodeResult result;

    fbzz::ai::BTNodeDef node;
    node.id      = asset.nextNodeId++;
    node.type    = type;
    node.name    = name.empty() ? fbzz::ai::BTNodeTypeName(type) : name;
    node.editorX = editorX;
    node.editorY = editorY;

    /// @note ルートがまだ無ければ、指定によらず最初のノードがルートになる。
    const bool hasRoot = std::any_of(asset.nodes.begin(), asset.nodes.end(),
        [](const fbzz::ai::BTNodeDef& item) { return item.parentId == 0; });
    node.parentId = 0;
    asset.nodes.push_back(node);
    result.nodeId = node.id;

    /// @note 最初のノード = ルート。親付けは不要。
    if (!hasRoot) return result;

    if (parentId == 0) {
        result.rejectReason = "ルートは既にあります。parentId を指定してください";
    } else {
        result.rejectReason = TryReparentNode(asset, node.id, parentId);
    }
    if (result.rejectReason.empty()) return result;

    if (orphanOnReject) {
        /// @note 孤立ノードとして残す。作った直後に消えると「追加できなかった」のか
        ///       「見えていない」のか区別できない。ルートが 2 つになるのは Validate が拒否し、
        ///       理由は警告バナーに出る。
        result.leftOrphan = true;
        return result;
    }

    /// @note 追加そのものを取り消す。呼び出しが成否で完結してほしい API 経路向け。
    std::erase_if(asset.nodes,
        [id = node.id](const fbzz::ai::BTNodeDef& item) { return item.id == id; });
    /// @note 採番も戻して id に穴を空けない
    asset.nextNodeId = node.id;
    result.nodeId = 0;
    return result;
}

int RemoveSubtree(fbzz::ai::BehaviorTreeAsset& asset, int nodeId)
{
    if (asset.FindNode(nodeId) == nullptr) return 0;

    const std::vector<GraphEdge> edges = ParentEdges(asset);
    const std::vector<int> doomed = CollectReachable(std::vector<int>{ nodeId }, edges);
    const std::unordered_set<int> doomedSet(doomed.begin(), doomed.end());

    const std::size_t before = asset.nodes.size();
    std::erase_if(asset.nodes, [&doomedSet](const fbzz::ai::BTNodeDef& node) {
        return doomedSet.contains(node.id);
    });
    return static_cast<int>(before - asset.nodes.size());
}

DuplicateResult DuplicateSubtree(fbzz::ai::BehaviorTreeAsset& asset,
                                 int   nodeId,
                                 float offsetX,
                                 float offsetY,
                                 int   parentOverride)
{
    DuplicateResult result;

    const fbzz::ai::BTNodeDef* source = asset.FindNode(nodeId);
    if (source == nullptr) {
        result.rejectReason = "ノードが見つかりません";
        return result;
    }
    if (source->parentId == 0) {
        result.rejectReason = "ルートは複製できません (木にルートは 1 つだけです)";
        return result;
    }
    const int targetParent = parentOverride != 0 ? parentOverride : source->parentId;

    const std::vector<GraphEdge> edges = ParentEdges(asset);
    const std::vector<int> subtree = CollectReachable(std::vector<int>{ nodeId }, edges);

    /// @note id の再割当と内部リンクの保持は framework の共通実装に任せる。
    ///       クリップボード・Template 取り込み・レイヤー複製と同じ規則で動く。
    int nextId = asset.nextNodeId - 1;
    const GraphExtractResult extracted = ExtractSubgraph(subtree, edges, nextId);
    asset.nextNodeId = nextId + 1;

    std::vector<fbzz::ai::BTNodeDef> copies;
    for (const auto& node : asset.nodes) {
        const auto mapped = extracted.idMap.find(node.id);
        if (mapped == extracted.idMap.end()) continue;
        fbzz::ai::BTNodeDef copy = node;
        copy.id = mapped->second;
        const auto mappedParent = extracted.idMap.find(node.parentId);
        /// @note 部分木の根だけは元の親のまま (兄弟として並ぶ)。
        copy.parentId = mappedParent == extracted.idMap.end() ? node.parentId
                                                              : mappedParent->second;
        copy.editorX += offsetX;
        copy.editorY += offsetY;
        copies.push_back(std::move(copy));
    }
    for (auto& copy : copies) asset.nodes.push_back(std::move(copy));

    result.idMap = extracted.idMap;

    /// @note 複製した根を親の末尾へ回す (order を採り直す)。
    const auto rootCopy = extracted.idMap.find(nodeId);
    if (rootCopy != extracted.idMap.end()) {
        result.newRootId    = rootCopy->second;
        result.rejectReason = TryReparentNode(asset, rootCopy->second, targetParent);
    }
    return result;
}

void AutoLayout(fbzz::ai::BehaviorTreeAsset& asset)
{
    if (asset.nodes.empty()) return;

    std::vector<int> nodeIds;
    std::vector<GraphLayoutEdge> edges;
    for (const auto& node : asset.nodes) {
        nodeIds.push_back(node.id);
        if (node.parentId != 0) edges.push_back({ node.parentId, node.id });
    }

    /// @note 木なので列 = 深さがそのまま階層になり、DAG より整った結果になる。
    const std::vector<int> roots = asset.FindRootIds();
    GraphLayoutOptions options;
    options.columnStep = kNodeColumnStep;
    options.rowStep    = kNodeRowStep;
    const auto layout = roots.empty() ? ComputeGraphLayout(nodeIds, edges, options)
                                      : ComputeGraphLayout(nodeIds, edges, roots, options);

    for (auto& node : asset.nodes) {
        const auto found = layout.find(node.id);
        if (found == layout.end()) continue;
        node.editorX = found->second.x;
        node.editorY = found->second.y;
    }
}

} // namespace fbzz::editor::btops
