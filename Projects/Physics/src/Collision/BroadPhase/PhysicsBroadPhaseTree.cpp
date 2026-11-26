/// @file    PhysicsBroadPhaseTree.cpp
/// @brief   保持 BVH の構築、境界変更の検出と差分更新。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "PhysicsBroadPhaseState.hpp"
#include <algorithm>
#include <cmath>

namespace fbzz::physics {
    namespace {
        float AxisValue(const math::Vector3& v, int axis)
        {
            if (axis == 0) {
                return v.x;
            }
            if (axis == 1) {
                return v.y;
            }
            return v.z;
        }

        int LongestAxis(const math::Vector3& size)
        {
            if (size.x >= size.y && size.x >= size.z) {
                return 0;
            }
            if (size.y >= size.z) {
                return 1;
            }
            return 2;
        }

        AABB MergeBounds(const AABB& a, const AABB& b)
        {
            AABB out;
            out.min.x = std::min(a.min.x, b.min.x);
            out.min.y = std::min(a.min.y, b.min.y);
            out.min.z = std::min(a.min.z, b.min.z);
            out.max.x = std::max(a.max.x, b.max.x);
            out.max.y = std::max(a.max.y, b.max.y);
            out.max.z = std::max(a.max.z, b.max.z);
            return out;
        }

        AABB ComputeBounds(const std::vector<BroadPhaseProxy>& proxies,
                           const std::vector<int>& proxyOrder,
                           int start,
                           int count)
        {
            AABB bounds = proxies[static_cast<size_t>(proxyOrder[static_cast<size_t>(start)])].bounds;
            for (int i = 1; i < count; ++i)
            {
                const int proxyIndex = proxyOrder[static_cast<size_t>(start + i)];
                bounds = MergeBounds(bounds, proxies[static_cast<size_t>(proxyIndex)].bounds);
            }
            return bounds;
        }

        AABB ComputeCenterBounds(const std::vector<BroadPhaseProxy>& proxies,
                                 const std::vector<int>& proxyOrder,
                                 int start,
                                 int count)
        {
            const math::Vector3 first = proxies[static_cast<size_t>(proxyOrder[static_cast<size_t>(start)])].center;
            AABB bounds{ first, first };
            for (int i = 1; i < count; ++i)
            {
                const math::Vector3 center = proxies[static_cast<size_t>(proxyOrder[static_cast<size_t>(start + i)])].center;
                bounds.min.x = std::min(bounds.min.x, center.x);
                bounds.min.y = std::min(bounds.min.y, center.y);
                bounds.min.z = std::min(bounds.min.z, center.z);
                bounds.max.x = std::max(bounds.max.x, center.x);
                bounds.max.y = std::max(bounds.max.y, center.y);
                bounds.max.z = std::max(bounds.max.z, center.z);
            }
            return bounds;
        }

        int BuildBroadPhaseBVH(const std::vector<BroadPhaseProxy>& proxies,
                               std::vector<int>& proxyOrder,
                               std::vector<BroadPhaseNode>& nodes,
                               int start,
                               int count, int parent = -1)
        {
            constexpr int LEAF_SIZE = 4;

            const int nodeIndex = static_cast<int>(nodes.size());
            nodes.push_back({});

            BroadPhaseNode& node = nodes[static_cast<size_t>(nodeIndex)];
            node.bounds = ComputeBounds(proxies, proxyOrder, start, count);
            node.start = start;
            node.count = count;
            node.parent = parent;

            if (count <= LEAF_SIZE) {
                return nodeIndex;
            }

            const AABB centerBounds = ComputeCenterBounds(proxies, proxyOrder, start, count);
            const math::Vector3 centerSize = centerBounds.max - centerBounds.min;
            const int splitAxis = LongestAxis(centerSize);
            const int split = start + count / 2;

            // Rebuild on membership changes or quality degradation uses median splitting instead of SAH.
            // 構築を O(n log n) に抑えつつ、総当たりより候補ペア数を安定して削減する。
            std::nth_element(proxyOrder.begin() + start,
                             proxyOrder.begin() + split,
                             proxyOrder.begin() + start + count,
                             [&](int a, int b) {
                                 return AxisValue(proxies[static_cast<size_t>(a)].center, splitAxis) <
                                        AxisValue(proxies[static_cast<size_t>(b)].center, splitAxis);
                             });

            // 全中心が同一点に潰れても split は count/2 で進むため、再帰は必ず収束する。
            node.left = BuildBroadPhaseBVH(proxies, proxyOrder, nodes, start, split - start, nodeIndex);
            node.right = BuildBroadPhaseBVH(proxies, proxyOrder, nodes, split, start + count - split, nodeIndex);
            return nodeIndex;
        }

        bool SameBroadPhaseBounds(const AABB& a, const AABB& b)
        {
            return a.min.x == b.min.x && a.min.y == b.min.y && a.min.z == b.min.z
                && a.max.x == b.max.x && a.max.y == b.max.y && a.max.z == b.max.z;
        }

        double BroadPhaseArea(const AABB& bounds)
        {
            const double x = static_cast<double>(bounds.max.x) - bounds.min.x;
            const double y = static_cast<double>(bounds.max.y) - bounds.min.y;
            const double z = static_cast<double>(bounds.max.z) - bounds.min.z;
            return 2.0 * (x * y + y * z + z * x);
        }
    }

    /// @see https://github.com/godotengine/godot/blob/master/modules/godot_physics_3d/godot_broad_phase_3d_bvh.cpp Godot の保持 BVH と移動時の更新。
    void PhysicsSolver::BroadPhaseState::Update(const std::vector<ColliderInstance>& colliders)
    {
        dirtyNodeIndices.clear();
        changedProxyIndices.clear();
        bool rebuild = nodes.empty() || !allProxyBoundsFinite;
        bool allBoundsFinite = true;
        std::size_t escapes = 0;
        const std::size_t escapeLimit = std::max(std::size_t{8}, proxies.size() / 8);
        std::size_t validCount = 0;

        // 入力順で境界を更新する。構成が同じ間は、境界の変化と既存の葉からはみ出した数だけを記録する。
        for (std::size_t index = 0; index < colliders.size(); ++index) {
            if (!colliders[index].collider) {
                continue;
            }
            const AABB bounds = colliders[index].collider->GetAABB();
            allBoundsFinite &= FiniteBroadPhaseBounds(bounds);
            if (validCount == proxies.size()) {
                proxies.push_back({index, bounds, bounds.Center()});
                rebuild = true;
            } else {
                auto& proxy = proxies[validCount];
                if (proxy.colliderIndex != index) {
                    rebuild = true;
                }
                if (!SameBroadPhaseBounds(proxy.bounds, bounds)) {
                    proxy.bounds = bounds;
                    proxy.center = bounds.Center();
                    if (!rebuild) {
                        changedProxyIndices.push_back(static_cast<int>(validCount));
                        if (!nodes[static_cast<std::size_t>(proxy.leafIndex)].bounds.Overlaps(bounds)) {
                            ++escapes;
                        }
                        rebuild = escapes > escapeLimit;
                    }
                }
                proxy.colliderIndex = index;
            }
            ++validCount;
        }
        if (validCount != proxies.size()) {
            proxies.resize(validCount);
            rebuild = true;
        }

        // 木の構成が変わらない場合は変更された葉だけを更新し、境界の重なりが増えすぎたら木を作り直す。
        allProxyBoundsFinite = allBoundsFinite;
        if (proxies.size() < 2 || !allProxyBoundsFinite) {
            nodes.clear();
            return;
        }

        if (rebuild) {
            Rebuild();
        } else if (!changedProxyIndices.empty()) {
            RefitChangedLeaves();
            if (NormalizedArea() > normalizedAreaAtBuild * 1.5) {
                Rebuild();
            }
        }
    }

    void PhysicsSolver::BroadPhaseState::RefitChangedLeaves()
    {
        for (int index : changedProxyIndices) {
            int current = proxies[static_cast<std::size_t>(index)].leafIndex;
            while (current >= 0 && !nodes[static_cast<std::size_t>(current)].dirty) {
                auto& node = nodes[static_cast<std::size_t>(current)];
                node.dirty = true;
                dirtyNodeIndices.push_back(current);
                current = node.parent;
            }
        }

        const auto refit = [&](int index) {
            auto& node = nodes[static_cast<std::size_t>(index)];
            if (!node.dirty) {
                return;
            }
            if (!node.IsLeaf()) {
                internalNodeArea -= BroadPhaseArea(node.bounds);
            }
            node.bounds = node.IsLeaf() ? ComputeBounds(proxies, proxyOrder, node.start, node.count)
                : MergeBounds(nodes[static_cast<std::size_t>(node.left)].bounds,
                    nodes[static_cast<std::size_t>(node.right)].bounds);
            if (!node.IsLeaf()) {
                internalNodeArea += BroadPhaseArea(node.bounds);
            }
            node.dirty = false;
        };
        // preorderの添字を逆順に処理すれば、共有祖先も子の更新後に一度だけ反映できる。
        if (dirtyNodeIndices.size() > nodes.size() / 4) {
            for (std::size_t index = nodes.size(); index > 0; --index) {
                refit(static_cast<int>(index - 1));
            }
        } else {
            std::sort(dirtyNodeIndices.begin(), dirtyNodeIndices.end(), std::greater<int>{});
            for (int index : dirtyNodeIndices) {
                refit(index);
            }
        }
    }

    double PhysicsSolver::BroadPhaseState::NormalizedArea() const
    {
        const double rootArea = BroadPhaseArea(nodes.front().bounds);
        return rootArea > 0.0 ? internalNodeArea / rootArea : 0.0;
    }

    void PhysicsSolver::BroadPhaseState::Rebuild()
    {
        proxyOrder.resize(proxies.size());
        for (std::size_t index = 0; index < proxyOrder.size(); ++index) {
            proxyOrder[index] = static_cast<int>(index);
        }
        nodes.clear();
        nodes.reserve(proxies.size() * 2);
        BuildBroadPhaseBVH(proxies, proxyOrder, nodes, 0, static_cast<int>(proxies.size()));
        internalNodeArea = 0.0;
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            const auto& node = nodes[index];
            if (!node.IsLeaf()) {
                internalNodeArea += BroadPhaseArea(node.bounds);
                continue;
            }
            for (int offset = 0; offset < node.count; ++offset) {
                proxies[static_cast<std::size_t>(proxyOrder[static_cast<std::size_t>(node.start + offset)])].leafIndex = static_cast<int>(index);
            }
        }
        normalizedAreaAtBuild = NormalizedArea();
    }
}
