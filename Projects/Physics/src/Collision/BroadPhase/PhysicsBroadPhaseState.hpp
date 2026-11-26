/// @file    PhysicsBroadPhaseState.hpp
/// @brief   広域衝突判定の保持 BVH と候補列挙の内部境界。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once
#include <Physics/Collision/NarrowPhase/PhysicsSolver.hpp>
#include <cmath>

namespace fbzz::physics {
    struct BroadPhaseProxy
    {
        size_t colliderIndex = 0; ///< この境界に対応する登録コライダー配列の添字。
        AABB bounds; ///< コライダーのワールド AABB [m]。
        math::Vector3 center = math::Vector3::ZERO; ///< BVH の分割に使う AABB のワールド中心 [m]。
        int leafIndex = -1; ///< この境界を格納する葉の nodes 内の添字。
    };

    struct BroadPhaseNode
    {
        AABB bounds; ///< 配下にあるコライダーを包むワールド AABB [m]。
        int left = -1; ///< 左の子の nodes 内の添字。子がなければ -1。
        int right = -1; ///< 右の子の nodes 内の添字。子がなければ -1。
        int start = 0; ///< この部分木が受け持つ proxyOrder の区間の先頭。
        int count = 0; ///< この部分木が受け持つコライダー境界の件数。
        int parent = -1; ///< 親ノードの nodes 内の添字。根なら -1。
        bool dirty = false; ///< 紐づく境界が変わり、このノードの範囲を再計算する必要があるかどうか。

        bool IsLeaf() const { return left < 0 && right < 0; }
    };

    inline bool FiniteBroadPhaseBounds(const AABB& bounds)
    {
        return std::isfinite(bounds.min.x) && std::isfinite(bounds.min.y) && std::isfinite(bounds.min.z)
            && std::isfinite(bounds.max.x) && std::isfinite(bounds.max.y) && std::isfinite(bounds.max.z)
            && bounds.min.x <= bounds.max.x && bounds.min.y <= bounds.max.y && bounds.min.z <= bounds.max.z;
    }

    struct PhysicsSolver::BroadPhaseState
    {
        void Update(const std::vector<ColliderInstance>& colliders);
        void CollectPairs(const std::vector<ColliderInstance>& colliders, std::vector<CollisionPair>& outPairs,
                          const std::function<bool(int, int)>& layerFilter) const;

        std::vector<BroadPhaseProxy> proxies; ///< 登録コライダーごとの境界を格納する配列。
        std::vector<int> proxyOrder; ///< BVH に格納する順序で並べ替える proxies の添字。
        std::vector<BroadPhaseNode> nodes; ///< 広域判定用の BVH ノードを格納する配列。
        std::vector<int> dirtyNodeIndices; ///< 境界の再計算が必要な nodes の添字。
        std::vector<int> changedProxyIndices; ///< 前回から境界が変化した proxies の添字。
        double internalNodeArea = 0.0; ///< 内部ノードの境界面積の合計。
        double normalizedAreaAtBuild = 0.0; ///< 再構築直後の正規化面積。木の質の診断に使う。
        bool allProxyBoundsFinite = true; ///< すべてのコライダー境界が BVH に格納可能かどうか。

    private:
        void RefitChangedLeaves();
        double NormalizedArea() const;
        void Rebuild();
    };
}
