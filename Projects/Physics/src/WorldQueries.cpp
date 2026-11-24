/// @file    WorldQueries.cpp
/// @brief   レイキャストの候補検索と全ヒットの距離順整列、クエリの走査境界。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "WorldQueryCast.hpp"
#include "WorldQueryGeometry.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace fbzz::physics {

AABB world_query_detail::SweptQueryBounds(const math::Vector3& origin, const math::Vector3& unitDirection,
    float maxDistance, float radius)
{
    const math::Vector3 end = origin + unitDirection * maxDistance;
    const float extent = std::abs(radius);
    const math::Vector3 expansion(extent, extent, extent);
    const AABB queryBounds{
        math::Vector3(std::min(origin.x, end.x), std::min(origin.y, end.y), std::min(origin.z, end.z)) - expansion,
        math::Vector3(std::max(origin.x, end.x), std::max(origin.y, end.y), std::max(origin.z, end.z)) + expansion,
    };
    if (HasFiniteQueryBounds(queryBounds) && std::isfinite(maxDistance) && maxDistance >= 0.0f) {
        return queryBounds;
    }

    const float infinity = std::numeric_limits<float>::infinity();
    return {{-infinity, -infinity, -infinity}, {infinity, infinity, infinity}};
}

namespace {

using world_query_detail::CastPath;

bool RaycastCandidate(const CastPath& path, const ColliderInstance& candidate, const World::ColliderFilter& filter,
    float& hitDistance, math::Vector3& hitNormal)
{
    return (!filter || filter(candidate))
        && world_query_detail::RaycastInstance(
            path.origin, path.unitDirection, path.maxDistance, candidate, hitDistance, hitNormal);
}

} /// @note namespace

using world_query_detail::MakeCastHit;
using world_query_detail::MakeCastPath;
using world_query_detail::NearestCastHit;
using world_query_detail::SweptQueryBounds;

bool World::Raycast(const math::Vector3& origin, const math::Vector3& direction, float maxDistance,
    RaycastHit& outHit, ColliderFilter filter) const
{
    const auto path = MakeCastPath(origin, direction, maxDistance);
    if (!path) {
        return false;
    }

    NearestCastHit nearestHit;
    const AABB queryBounds = SweptQueryBounds(path->origin, path->unitDirection, path->maxDistance, 0.0f);
    ForEachQueryCandidate(queryBounds, [&](size_t colliderIndex, const ColliderInstance& candidate) {
        float hitDistance;
        math::Vector3 hitNormal;
        if (RaycastCandidate(*path, candidate, filter, hitDistance, hitNormal)) {
            nearestHit.Consider(*path, colliderIndex, candidate, hitDistance, hitNormal);
        }
    });
    return nearestHit.WriteResult(path->maxDistance, outHit);
}

std::vector<World::RaycastHit> World::RaycastAll(const math::Vector3& origin, const math::Vector3& direction,
    float maxDistance, ColliderFilter filter) const
{
    const auto path = MakeCastPath(origin, direction, maxDistance);
    if (!path) {
        return {};
    }

    std::vector<RaycastHit> hits;
    const AABB queryBounds = SweptQueryBounds(path->origin, path->unitDirection, path->maxDistance, 0.0f);
    ForEachQueryCandidate(queryBounds, [&](size_t, const ColliderInstance& candidate) {
        float hitDistance;
        math::Vector3 hitNormal;
        if (RaycastCandidate(*path, candidate, filter, hitDistance, hitNormal)) {
            hits.push_back(MakeCastHit(*path, candidate, hitDistance, hitNormal));
        }
    });

    std::sort(hits.begin(), hits.end(), [](const RaycastHit& leftHit, const RaycastHit& rightHit) {
        return leftHit.distance < rightHit.distance;
    });
    return hits;
}

} /// @note namespace fbzz::physics
