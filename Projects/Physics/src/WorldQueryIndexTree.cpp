/// @file    WorldQueryIndexTree.cpp
/// @brief   クエリ用の平坦な BVH の構築と境界更新。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/World.hpp>
#include <algorithm>
#include <numeric>

namespace fbzz::physics {

void World::RebuildQueryNodes()
{
    const size_t colliderCount = m_indexedColliderBounds.size();
    m_queryBuildOrder.resize(colliderCount);
    std::iota(m_queryBuildOrder.begin(), m_queryBuildOrder.end(), size_t{0});
    m_queryNodes.clear();
    m_queryNodes.reserve(colliderCount * 2 - 1);
    BuildQueryNode(0, colliderCount);
}

void World::RefitQueryNodes()
{
    /// @note 親が子より先に並ぶため、逆順なら更新済みの子の境界を使える。
    for (size_t remainingNodes = m_queryNodes.size(); remainingNodes > 0; --remainingNodes) {
        const size_t nodeIndex = remainingNodes - 1;
        QueryNode& node = m_queryNodes[nodeIndex];
        if (node.colliderIndex != SIZE_MAX) {
            node.bounds = m_indexedColliderBounds[node.colliderIndex];
            continue;
        }

        const QueryNode& leftChild = m_queryNodes[nodeIndex + 1];
        const QueryNode& rightChild = m_queryNodes[leftChild.nextAfterSubtree];
        node.bounds = leftChild.bounds.Merge(rightChild.bounds);
    }
}

/// @see https://pbr-book.org/4ed/Primitives_and_Intersection_Acceleration/Bounding_Volume_Hierarchies PBRT 7.3.1: EqualCounts の分割と flat BVH。
size_t World::BuildQueryNode(size_t rangeBegin, size_t rangeEnd)
{
    const size_t nodeIndex = m_queryNodes.size();
    m_queryNodes.emplace_back();

    AABB nodeBounds = m_indexedColliderBounds[m_queryBuildOrder[rangeBegin]];
    for (size_t orderIndex = rangeBegin + 1; orderIndex < rangeEnd; ++orderIndex) {
        nodeBounds = nodeBounds.Merge(m_indexedColliderBounds[m_queryBuildOrder[orderIndex]]);
    }
    m_queryNodes[nodeIndex].bounds = nodeBounds;

    const size_t leafCount = rangeEnd - rangeBegin;
    if (leafCount == 1) {
        m_queryNodes[nodeIndex].colliderIndex = m_queryBuildOrder[rangeBegin];
    } else {
        const math::Vector3 extent = nodeBounds.max - nodeBounds.min;
        const int splitAxis = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
        const auto centerAlongSplitAxis = [this, splitAxis](size_t colliderIndex) {
            const AABB& bounds = m_indexedColliderBounds[colliderIndex];
            /// @note 加算前に半分にして、同符号の大きな境界値でも中心座標のオーバーフローを避ける。
            if (splitAxis == 0) return bounds.min.x * 0.5f + bounds.max.x * 0.5f;
            if (splitAxis == 1) return bounds.min.y * 0.5f + bounds.max.y * 0.5f;
            return bounds.min.z * 0.5f + bounds.max.z * 0.5f;
        };

        const size_t medianIndex = rangeBegin + leafCount / 2;
        const auto buildOrder = m_queryBuildOrder.begin();
        std::nth_element(buildOrder + rangeBegin, buildOrder + medianIndex, buildOrder + rangeEnd,
            [&centerAlongSplitAxis](size_t leftColliderIndex, size_t rightColliderIndex) {
                const float leftCenter = centerAlongSplitAxis(leftColliderIndex);
                const float rightCenter = centerAlongSplitAxis(rightColliderIndex);
                return leftCenter < rightCenter
                    || (leftCenter == rightCenter && leftColliderIndex < rightColliderIndex);
            });
        BuildQueryNode(rangeBegin, medianIndex);
        BuildQueryNode(medianIndex, rangeEnd);
    }

    m_queryNodes[nodeIndex].nextAfterSubtree = m_queryNodes.size();
    return nodeIndex;
}

} /// @note namespace fbzz::physics
