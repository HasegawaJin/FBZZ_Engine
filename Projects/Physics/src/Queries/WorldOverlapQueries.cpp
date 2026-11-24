/// @file    WorldOverlapQueries.cpp
/// @brief   球と重なるコライダーの候補検索と列挙。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/World/World.hpp>
#include "WorldQueryGeometry.hpp"

namespace fbzz::physics {

std::vector<const ColliderInstance*> World::OverlapSphere(
    const math::Vector3& center, float radius, ColliderFilter filter) const
{
    const AABB queryBounds = world_query_detail::SweptQueryBounds(center, math::Vector3::ZERO, 0.0f, radius);
    std::vector<const ColliderInstance*> overlappingColliders;
    ForEachQueryCandidate(queryBounds, [&](size_t, const ColliderInstance& candidate) {
        if (filter && !filter(candidate)) {
            return;
        }
        if (world_query_detail::SphereOverlapsInstance(center, radius, candidate)) {
            overlappingColliders.push_back(&candidate);
        }
    });
    return overlappingColliders;
}

} /// @note namespace fbzz::physics
