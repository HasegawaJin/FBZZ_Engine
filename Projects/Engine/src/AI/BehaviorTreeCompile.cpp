/// @file    BehaviorTreeCompile.cpp
/// @brief   オーサリング表現 (id / parentId / order) → ランタイム表現 (DFS pre-order フラット配列)。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Engine/AI/BehaviorTreeRuntime.hpp>

#include <algorithm>
#include <unordered_map>

namespace fbzz::ai {

namespace {

void SetError(std::string* outError, const std::string& message)
{
    if (outError) *outError = message;
}

// 予約キーを先頭に固定した Blackboard 定義列を作る。
//
// WHY アセットの blackboard をそのまま使わないか:
//   手書きの .behaviortree や古いアセットでは予約キーが欠けている / 順序が違う
//   可能性がある。bb::Self 等の固定添字はランタイムの前提なので、
//   ここで必ず成立させる。ユーザー定義キーは予約キーの後ろへ寄せる。
std::vector<BlackboardDef> BuildBlackboard(const BehaviorTreeAsset& asset)
{
    std::vector<BlackboardDef> result = ReservedBlackboardDefs();

    for (const BlackboardDef& def : asset.blackboard) {
        // 予約キーと同名のものはアセット側の定義を捨てる (型を勝手に変えられると壊れる)。
        const bool isReserved = std::any_of(result.begin(), result.end(),
            [&def](const BlackboardDef& reserved) { return reserved.name == def.name; });
        if (isReserved) continue;
        if (def.name.empty()) continue;

        result.push_back(def);
    }
    return result;
}

BlackboardKey ResolveKey(const std::vector<BlackboardDef>& defs, const std::string& name)
{
    if (name.empty()) return kInvalidBlackboardKey;
    for (std::size_t i = 0; i < defs.size(); ++i) {
        if (defs[i].name == name) return static_cast<BlackboardKey>(i);
    }
    return kInvalidBlackboardKey;
}

BTNodeParams MakeParams(const BTNodeDef& def, const std::vector<BlackboardDef>& blackboard,
                        std::vector<std::string>& warnings)
{
    BTNodeParams params;
    params.abortMode     = def.abortMode;
    params.compareOp     = def.compareOp;
    params.valueType     = def.valueType;
    params.successPolicy = def.successPolicy;

    params.repeatCount        = def.repeatCount;
    params.repeatUntilFailure = def.repeatUntilFailure;
    params.duration           = def.duration;
    params.durationRandom     = def.durationRandom;
    params.acceptanceRadius   = def.acceptanceRadius;
    params.chaseEntity        = def.chaseEntity;
    params.repathInterval     = def.repathInterval;
    params.range              = def.range;
    params.turnSpeedDeg       = def.turnSpeedDeg;
    params.threshold01        = def.threshold01;
    params.withinSeconds      = def.withinSeconds;
    params.volume             = def.volume;
    params.waitForAnimation   = def.waitForAnimation;

    params.valueBool    = def.valueBool;
    params.valueInt     = def.valueInt;
    params.valueFloat   = def.valueFloat;
    params.valueVector3 = def.valueVector3;
    params.valueString  = def.valueString;
    params.childWeights = def.childWeights;
    params.authoringId  = def.id;

    // 種別ごとに 1 本しか使わない文字列を text に畳む。
    switch (def.type) {
    case BTNodeType::PlayAnimation: params.text = def.animatorTrigger; break;
    case BTNodeType::PlayAudio:     params.text = def.soundPath;       break;
    case BTNodeType::RunScript:     params.text = def.scriptMethod;    break;
    default: break;
    }

    // Blackboard キーの解決。失敗しても compile を落とさず警告に積む。
    if (!def.keyName.empty()) {
        params.key = ResolveKey(blackboard, def.keyName);
        if (params.key == kInvalidBlackboardKey) {
            warnings.push_back("Blackboard キー \"" + def.keyName + "\" を解決できません (node id "
                               + std::to_string(def.id) + ")");
        }
    }
    if (!def.moveTargetKey.empty()) {
        params.moveKey = ResolveKey(blackboard, def.moveTargetKey);
        if (params.moveKey == kInvalidBlackboardKey) {
            warnings.push_back("Blackboard キー \"" + def.moveTargetKey + "\" を解決できません (node id "
                               + std::to_string(def.id) + ")");
        }
    }
    return params;
}

} // namespace

BlackboardKey BehaviorTreeRuntime::FindKey(std::string_view name) const
{
    for (std::size_t i = 0; i < keyNames.size(); ++i) {
        if (keyNames[i] == name) return static_cast<BlackboardKey>(i);
    }
    return kInvalidBlackboardKey;
}

bool CompileBehaviorTree(const BehaviorTreeAsset& asset, BehaviorTreeRuntime& out,
                         std::string* outError)
{
    out = BehaviorTreeRuntime{};

    if (asset.nodes.empty()) {
        SetError(outError, "ノードが 1 つもありません");
        return false;
    }
    if (asset.nodes.size() > 0xFFFEu) {
        SetError(outError, "ノード数が上限 (65534) を超えています");
        return false;
    }

    // ── id → アセット添字 ────────────────────────────────────────────────────
    std::unordered_map<int, std::size_t> indexOfId;
    indexOfId.reserve(asset.nodes.size());
    for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
        const BTNodeDef& def = asset.nodes[i];
        if (def.id == 0) {
            SetError(outError, "id = 0 のノードがあります (0 は無効値として予約)");
            return false;
        }
        if (!indexOfId.emplace(def.id, i).second) {
            SetError(outError, "id が重複しています: " + std::to_string(def.id));
            return false;
        }
    }

    // ── 子リストの構築 (order 昇順、同値は id 昇順で安定化) ──────────────────
    std::vector<std::vector<std::size_t>> children(asset.nodes.size());
    std::vector<std::size_t> roots;

    for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
        const BTNodeDef& def = asset.nodes[i];
        if (def.parentId == 0) { roots.push_back(i); continue; }

        const auto it = indexOfId.find(def.parentId);
        if (it == indexOfId.end()) {
            SetError(outError, "親 id " + std::to_string(def.parentId) + " が存在しません (node id "
                               + std::to_string(def.id) + ")");
            return false;
        }
        children[it->second].push_back(i);
    }

    if (roots.empty()) {
        SetError(outError, "ルートノードがありません (親を持たないノードが 0 個)");
        return false;
    }
    if (roots.size() > 1) {
        SetError(outError, "ルートノードが " + std::to_string(roots.size())
                           + " 個あります (1 個でなければなりません)");
        return false;
    }

    for (std::size_t i = 0; i < children.size(); ++i) {
        auto& list = children[i];
        std::sort(list.begin(), list.end(), [&asset](std::size_t a, std::size_t b) {
            if (asset.nodes[a].order != asset.nodes[b].order)
                return asset.nodes[a].order < asset.nodes[b].order;
            return asset.nodes[a].id < asset.nodes[b].id;
        });

        const int maxChildren = BTNodeMaxChildren(asset.nodes[i].type);
        if (maxChildren >= 0 && static_cast<int>(list.size()) > maxChildren) {
            SetError(outError, std::string(BTNodeTypeName(asset.nodes[i].type))
                               + " は子を " + std::to_string(maxChildren)
                               + " 個までしか持てません (node id "
                               + std::to_string(asset.nodes[i].id) + ")");
            return false;
        }
    }

    // ── DFS pre-order でフラット化 ──────────────────────────────────────────
    // WHY pre-order か: 「index が小さいほど左 = 高優先度」と
    //     「部分木が連続区間になる」の 2 つが同時に成立する唯一の順序。
    //     observerAborts の区間判定がこの性質に依存している。
    const std::vector<BlackboardDef> blackboard = BuildBlackboard(asset);

    out.nodes.reserve(asset.nodes.size());
    out.params.reserve(asset.nodes.size());
    out.parent.reserve(asset.nodes.size());
    out.subtreeEnd.reserve(asset.nodes.size());
    out.authoringIdOf.reserve(asset.nodes.size());

    std::vector<bool> visited(asset.nodes.size(), false);

    // 明示スタックによる DFS。再帰にしないのは深い木でのスタック溢れを避けるため。
    struct Frame {
        std::size_t   assetIndex;
        std::uint16_t emittedIndex;
        std::size_t   nextChild;
    };
    std::vector<Frame> stack;

    const auto emit = [&](std::size_t assetIndex, std::uint16_t parentIndex) -> std::uint16_t {
        const BTNodeDef& def = asset.nodes[assetIndex];
        const auto emitted = static_cast<std::uint16_t>(out.nodes.size());

        BTNode node;
        node.type       = def.type;
        node.firstChild = 0;
        node.childCount = 0;
        node.paramIndex = emitted;   // params は nodes と同じ並び
        out.nodes.push_back(node);

        out.params.push_back(MakeParams(def, blackboard, out.compileWarnings));
        out.parent.push_back(parentIndex);
        out.subtreeEnd.push_back(0);   // DFS の復路で確定させる
        out.authoringIdOf.push_back(def.id);
        return emitted;
    };

    visited[roots[0]] = true;
    stack.push_back({ roots[0], emit(roots[0], kInvalidNode), 0 });

    while (!stack.empty()) {
        Frame& frame = stack.back();
        const auto& childList = children[frame.assetIndex];

        if (frame.nextChild < childList.size()) {
            const std::size_t childAsset = childList[frame.nextChild];
            ++frame.nextChild;

            if (visited[childAsset]) {
                SetError(outError, "親子関係に循環があります (node id "
                                   + std::to_string(asset.nodes[childAsset].id) + ")");
                return false;
            }
            visited[childAsset] = true;

            const std::uint16_t childEmitted = emit(childAsset, frame.emittedIndex);

            // 最初の子を記録し、以降は連番になることを利用して個数だけ数える。
            BTNode& parentNode = out.nodes[frame.emittedIndex];
            if (parentNode.childCount == 0) parentNode.firstChild = childEmitted;
            ++parentNode.childCount;

            stack.push_back({ childAsset, childEmitted, 0 });
            continue;
        }

        // 全ての子を出し終えた = この部分木の終端が確定した。
        out.subtreeEnd[frame.emittedIndex] = static_cast<std::uint16_t>(out.nodes.size());
        stack.pop_back();
    }

    // 到達できなかったノードは木から切り離されている。落とさず警告に留める。
    // WHY: エディタで枝を一時的に外して試すことがあり、その状態で保存されうる。
    for (std::size_t i = 0; i < visited.size(); ++i) {
        if (!visited[i]) {
            out.compileWarnings.push_back("ルートから到達できないノードを無視しました (id "
                                          + std::to_string(asset.nodes[i].id) + ")");
        }
    }

    out.blackboard = blackboard;
    out.keyNames.reserve(blackboard.size());
    for (const BlackboardDef& def : blackboard) out.keyNames.push_back(def.name);

    // ── observerAborts の中断候補を事前計算 ────────────────────────────────
    const auto isPriorityComposite = [&out](std::uint16_t index) {
        const BTNodeType type = out.nodes[index].type;
        // 左→右の優先順位を持つのは Sequence / Selector だけ。
        // Parallel / RandomSelector は子の順序に意味がないので連鎖をここで打ち切る。
        return type == BTNodeType::Sequence || type == BTNodeType::Selector;
    };

    for (std::uint16_t i = 0; i < static_cast<std::uint16_t>(out.nodes.size()); ++i) {
        const BTNodeParams& params = out.params[out.nodes[i].paramIndex];
        if (params.abortMode == AbortMode::None) continue;

        // 監視条件が守る範囲を決める。
        //   Decorator の場合 : 自分の部分木 (子を含む)
        //   リーフ条件の場合 : 自分を含む親 Composite の部分木
        //
        // WHY リーフで親をスコープにするか:
        //   Sequence[HasTarget, Chase] と書いたとき、HasTarget は
        //   「この Sequence 全体を守るガード」として読むのが自然。
        //   自分自身 (1 ノード) をスコープにすると、Chase が Running 中に
        //   条件が偽へ落ちても中断されず、意図と食い違う。
        std::uint16_t scope = i;
        if (!BTNodeIsDecorator(out.nodes[i].type) && out.parent[i] != kInvalidNode)
            scope = out.parent[i];

        BTAbortCandidate candidate;
        candidate.node   = i;
        candidate.mode   = params.abortMode;
        candidate.selfLo = scope;
        candidate.selfHi = out.subtreeEnd[scope];

        // 自分より右 (低優先度) の領域を、優先度連鎖が続く限り上へ広げる。
        std::uint16_t lo = out.subtreeEnd[scope];
        std::uint16_t hi = out.subtreeEnd[scope];
        std::uint16_t cur = scope;
        while (out.parent[cur] != kInvalidNode) {
            const std::uint16_t parentIndex = out.parent[cur];
            if (!isPriorityComposite(parentIndex)) break;
            hi  = out.subtreeEnd[parentIndex];
            cur = parentIndex;
        }
        candidate.lowLo = lo;
        candidate.lowHi = hi;

        out.abortCandidates.push_back(candidate);
    }

    // node index 昇順 = 優先度の高い順。最初に成立した中断が勝つ。
    std::sort(out.abortCandidates.begin(), out.abortCandidates.end(),
              [](const BTAbortCandidate& a, const BTAbortCandidate& b) { return a.node < b.node; });

    return true;
}

} // namespace fbzz::ai
