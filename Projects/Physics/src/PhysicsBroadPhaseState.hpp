/// @file    PhysicsBroadPhaseState.hpp
/// @brief   広域衝突判定の保持 BVH と候補列挙の内部境界。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once
#include <Physics/PhysicsSolver.hpp>
#include <cmath>

namespace fbzz::physics {
    struct BroadPhaseProxy
    {
        size_t colliderIndex = 0;
        AABB bounds;
        math::Vector3 center = math::Vector3::ZERO;
        int leafIndex = -1;
    };

    struct BroadPhaseNode
    {
        AABB bounds;
        int left = -1;
        int right = -1;
        int start = 0;
        int count = 0;
        int parent = -1;
        bool dirty = false;

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

        std::vector<BroadPhaseProxy> proxies;
        std::vector<int> proxyOrder;
        std::vector<BroadPhaseNode> nodes;
        std::vector<int> dirtyNodeIndices;
        std::vector<int> changedProxyIndices;
        double internalNodeArea = 0.0;
        double normalizedAreaAtBuild = 0.0;
        bool allProxyBoundsFinite = true;

    private:
        void RefitChangedLeaves();
        double NormalizedArea() const;
        void Rebuild();
    };
}
