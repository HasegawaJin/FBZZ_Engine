/// @file    WorldSphereCast.cpp
/// @brief   単発・バッチの球キャストと最近接ヒットの選択。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "WorldQueryCast.hpp"
#include "WorldQueryGeometry.hpp"

namespace fbzz::physics {

using world_query_detail::MakeCastPath;
using world_query_detail::NearestCastHit;
using world_query_detail::RaycastExpandedCollider;
using world_query_detail::SweptQueryBounds;

bool World::SphereCast(const math::Vector3& origin, float radius, const math::Vector3& direction,
    float maxDistance, RaycastHit& outHit, ColliderFilter filter) const
{
    return SphereCastImpl(origin, radius, direction, maxDistance, outHit, filter, nullptr);
}

World::SphereCastBatch World::PrepareSphereCasts() const
{
    return SphereCastBatch(*this, QueryIndexMatchesColliders());
}

bool World::SphereCastBatch::Cast(const math::Vector3& origin, float radius, const math::Vector3& direction,
    float maxDistance, RaycastHit& outHit, const ColliderFilter& filter) const
{
    return m_world.SphereCastImpl(origin, radius, direction, maxDistance, outHit, filter, &m_useIndex);
}

bool World::SphereCastImpl(const math::Vector3& origin, float radius, const math::Vector3& direction,
    float maxDistance, RaycastHit& outHit, const ColliderFilter& filter, const bool* preparedIndex) const
{
    const auto path = MakeCastPath(origin, direction, maxDistance);
    if (!path) {
        return false;
    }

    NearestCastHit nearestHit;
    const AABB queryBounds = SweptQueryBounds(path->origin, path->unitDirection, path->maxDistance, radius);
    ForEachQueryCandidate(queryBounds, [&](size_t colliderIndex, const ColliderInstance& candidate) {
        if (!candidate.collider || (filter && !filter(candidate))) {
            return;
        }

        float hitDistance = -1.0f;
        math::Vector3 hitNormal;
        RaycastExpandedCollider(path->origin, path->unitDirection, path->maxDistance,
            candidate, radius, hitDistance, hitNormal);
        if (hitDistance >= 0.0f) {
            nearestHit.Consider(*path, colliderIndex, candidate, hitDistance, hitNormal);
        }
    }, preparedIndex);
    return nearestHit.WriteResult(path->maxDistance, outHit);
}

} /// @note namespace fbzz::physics
