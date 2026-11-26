/// @file    PhysicsBroadPhasePairs.cpp
/// @brief   現在のコライダー状態を使う広域衝突候補の列挙。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "PhysicsBroadPhaseState.hpp"

namespace fbzz::physics {
    namespace {
        bool ShouldSkipBroadPhasePair(const ColliderInstance& a,
                                       const ColliderInstance& b,
                                       const std::function<bool(int, int)>& layerFilter)
        {
            if (layerFilter && !layerFilter(a.layer, b.layer)) {
                return true;
            }

            // 同じ剛体に属するコライダー同士は当たらない。«胴+脚4本» のように 1 体を
            // 複数形状で表すと重なりは当然で、解こうとすると自分自身を押してしまう。
            // トリガー判定より先に見るのも、自分の一部への侵入通知に意味が無いため。
            if (a.body && a.body == b.body) {
                return true;
            }

            if (a.isTrigger || b.isTrigger) {
                return false;
            }

            // 非 Trigger の Static / Sleeping 同士は解決しても状態が変わらない。特に
            // Sleeping dynamic vs static TriangleMesh は、接触維持のためだけに Terrain BVH
            // クエリを毎 substep 実行し 1 フレーム数十 ms を消費しうる。
            const bool inactiveA = !a.body || a.body->IsStatic() || a.body->IsSleeping();
            const bool inactiveB = !b.body || b.body->IsStatic() || b.body->IsSleeping();
            return inactiveA && inactiveB;
        }

        void AddBroadPhasePair(const std::vector<ColliderInstance>& colliders,
                               const BroadPhaseProxy& a,
                               const BroadPhaseProxy& b,
                               std::vector<CollisionPair>& outPairs,
                               const std::function<bool(int, int)>& layerFilter)
        {
            const ColliderInstance& colliderA = colliders[a.colliderIndex];
            const ColliderInstance& colliderB = colliders[b.colliderIndex];
            if (ShouldSkipBroadPhasePair(colliderA, colliderB, layerFilter)) {
                return;
            }
            if (!a.bounds.Overlaps(b.bounds)) {
                return;
            }

            outPairs.push_back({ &colliderA, &colliderB });
        }

        void CollectLeafPairs(const std::vector<ColliderInstance>& colliders,
                              const std::vector<BroadPhaseProxy>& proxies,
                              const std::vector<int>& proxyOrder,
                              const BroadPhaseNode& node,
                              std::vector<CollisionPair>& outPairs,
                              const std::function<bool(int, int)>& layerFilter)
        {
            for (int i = 0; i < node.count; ++i)
            {
                const BroadPhaseProxy& a = proxies[static_cast<size_t>(proxyOrder[static_cast<size_t>(node.start + i)])];
                for (int j = i + 1; j < node.count; ++j)
                {
                    const BroadPhaseProxy& b = proxies[static_cast<size_t>(proxyOrder[static_cast<size_t>(node.start + j)])];
                    AddBroadPhasePair(colliders, a, b, outPairs, layerFilter);
                }
            }
        }

        void CollectNodePairs(const std::vector<ColliderInstance>& colliders,
                              const std::vector<BroadPhaseProxy>& proxies,
                              const std::vector<int>& proxyOrder,
                              const std::vector<BroadPhaseNode>& nodes,
                              int nodeAIndex,
                              int nodeBIndex,
                              std::vector<CollisionPair>& outPairs,
                              const std::function<bool(int, int)>& layerFilter)
        {
            const BroadPhaseNode& nodeA = nodes[static_cast<size_t>(nodeAIndex)];
            const BroadPhaseNode& nodeB = nodes[static_cast<size_t>(nodeBIndex)];
            if (!nodeA.bounds.Overlaps(nodeB.bounds)) {
                return;
            }

            if (nodeA.IsLeaf() && nodeB.IsLeaf())
            {
                for (int i = 0; i < nodeA.count; ++i)
                {
                    const BroadPhaseProxy& a = proxies[static_cast<size_t>(proxyOrder[static_cast<size_t>(nodeA.start + i)])];
                    for (int j = 0; j < nodeB.count; ++j)
                    {
                        const BroadPhaseProxy& b = proxies[static_cast<size_t>(proxyOrder[static_cast<size_t>(nodeB.start + j)])];
                        AddBroadPhasePair(colliders, a, b, outPairs, layerFilter);
                    }
                }
                return;
            }

            if (nodeB.IsLeaf() || (!nodeA.IsLeaf() && nodeA.count >= nodeB.count))
            {
                CollectNodePairs(colliders, proxies, proxyOrder, nodes, nodeA.left, nodeBIndex, outPairs, layerFilter);
                CollectNodePairs(colliders, proxies, proxyOrder, nodes, nodeA.right, nodeBIndex, outPairs, layerFilter);
                return;
            }

            CollectNodePairs(colliders, proxies, proxyOrder, nodes, nodeAIndex, nodeB.left, outPairs, layerFilter);
            CollectNodePairs(colliders, proxies, proxyOrder, nodes, nodeAIndex, nodeB.right, outPairs, layerFilter);
        }

        void CollectSelfPairs(const std::vector<ColliderInstance>& colliders,
                              const std::vector<BroadPhaseProxy>& proxies,
                              const std::vector<int>& proxyOrder,
                              const std::vector<BroadPhaseNode>& nodes,
                              int nodeIndex,
                              std::vector<CollisionPair>& outPairs,
                              const std::function<bool(int, int)>& layerFilter)
        {
            const BroadPhaseNode& node = nodes[static_cast<size_t>(nodeIndex)];
            if (node.IsLeaf())
            {
                CollectLeafPairs(colliders, proxies, proxyOrder, node, outPairs, layerFilter);
                return;
            }

            CollectSelfPairs(colliders, proxies, proxyOrder, nodes, node.left, outPairs, layerFilter);
            CollectNodePairs(colliders, proxies, proxyOrder, nodes, node.left, node.right, outPairs, layerFilter);
            CollectSelfPairs(colliders, proxies, proxyOrder, nodes, node.right, outPairs, layerFilter);
        }
    }

    void PhysicsSolver::BroadPhaseState::CollectPairs(const std::vector<ColliderInstance>& colliders,
        std::vector<CollisionPair>& outPairs, const std::function<bool(int, int)>& layerFilter) const
    {
        if (proxies.size() < 2) {
            return;
        }
        if (!allProxyBoundsFinite) {
            for (size_t first = 0; first < proxies.size(); ++first) {
                for (size_t second = first + 1; second < proxies.size(); ++second) {
                    AddBroadPhasePair(colliders, proxies[first], proxies[second], outPairs, layerFilter);
                }
            }
            return;
        }
        CollectSelfPairs(colliders, proxies, proxyOrder, nodes, 0, outPairs, layerFilter);
    }
}
