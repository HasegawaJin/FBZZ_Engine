/// @file    WorldQueryIndex.cpp
/// @brief   World のコライダーとクエリ索引の同期・整合性確認。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/World/World.hpp>
#include "WorldQueryGeometry.hpp"
#include <atomic>

namespace fbzz::physics {

namespace {

using world_query_detail::HasFiniteQueryBounds;

constexpr size_t MIN_INDEXED_COLLIDERS = 16;

bool BoundsMatchExactly(const AABB& indexedBounds, const AABB& currentBounds)
{
    return indexedBounds.min.x == currentBounds.min.x
        && indexedBounds.min.y == currentBounds.min.y
        && indexedBounds.min.z == currentBounds.min.z
        && indexedBounds.max.x == currentBounds.max.x
        && indexedBounds.max.y == currentBounds.max.y
        && indexedBounds.max.z == currentBounds.max.z;
}

} /// @note namespace

void World::RefreshQueryIndex()
{
    if (m_colliders.empty()) {
        m_queryNodes.clear();
        m_indexedColliderBounds.clear();
        m_queryBuildOrder.clear();
        m_indexedColliderIdentities.clear();
        return;
    }
    // query を使わない World と substep に索引維持の走査を追加しない。
    if (!std::atomic_ref<bool>(m_queryRefreshRequested).exchange(false, std::memory_order_relaxed)) {
        return;
    }

    switch (CaptureQueryIndexInputs()) {
    case QueryIndexUpdate::UNAVAILABLE:
        m_queryNodes.clear();
        break;
    case QueryIndexUpdate::REFIT:
        RefitQueryNodes();
        break;
    case QueryIndexUpdate::REBUILD:
        RebuildQueryNodes();
        break;
    }
}

World::QueryIndexUpdate World::CaptureQueryIndexInputs()
{
    const size_t colliderCount = m_colliders.size();
    bool canRefit = !m_queryNodes.empty() && m_indexedColliderIdentities.size() == colliderCount;
    bool allBoundsUsable = true;

    m_indexedColliderBounds.resize(colliderCount);
    for (size_t colliderIndex = 0; colliderIndex < colliderCount; ++colliderIndex) {
        const Collider* collider = m_colliders[colliderIndex].collider;
        canRefit &= colliderIndex < m_indexedColliderIdentities.size()
            && m_indexedColliderIdentities[colliderIndex] == collider;
        if (!IsLiveCollider(colliderIndex)) {
            allBoundsUsable = false;
            continue;
        }
        AABB& indexedBounds = m_indexedColliderBounds[colliderIndex];
        indexedBounds = collider->GetAABB();
        allBoundsUsable &= HasFiniteQueryBounds(indexedBounds);
    }

    m_indexedColliderIdentities.resize(colliderCount);
    for (size_t colliderIndex = 0; colliderIndex < colliderCount; ++colliderIndex) {
        m_indexedColliderIdentities[colliderIndex] = m_colliders[colliderIndex].collider;
    }

    // 少数の形状や不正な境界では、索引構築の負担と保守的判定の欠落を避ける。
    if (!allBoundsUsable || colliderCount < MIN_INDEXED_COLLIDERS) {
        return QueryIndexUpdate::UNAVAILABLE;
    }
    return canRefit ? QueryIndexUpdate::REFIT : QueryIndexUpdate::REBUILD;
}

bool World::QueryIndexMatchesColliders() const
{
    std::atomic_ref<bool>(m_queryRefreshRequested).store(true, std::memory_order_relaxed);
    if (m_queryNodes.empty() || m_indexedColliderBounds.size() != m_colliders.size()) {
        return false;
    }
    for (size_t colliderIndex = 0; colliderIndex < m_colliders.size(); ++colliderIndex) {
        if (!IsLiveCollider(colliderIndex)
            || !BoundsMatchExactly(m_indexedColliderBounds[colliderIndex], m_colliders[colliderIndex].collider->GetAABB())) {
            return false;
        }
    }
    return true;
}

} /// @note namespace fbzz::physics
