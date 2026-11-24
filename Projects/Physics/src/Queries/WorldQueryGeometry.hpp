/// @file    WorldQueryGeometry.hpp
/// @brief   World の候補索引と形状クエリで共有する境界と交差判定。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Physics/Collision/BroadPhase/CollisionPair.hpp>
#include <cmath>

namespace fbzz::physics::world_query_detail {

[[nodiscard]] inline bool HasFiniteQueryBounds(const AABB& bounds)
{
    return std::isfinite(bounds.min.x) && std::isfinite(bounds.min.y) && std::isfinite(bounds.min.z)
        && std::isfinite(bounds.max.x) && std::isfinite(bounds.max.y) && std::isfinite(bounds.max.z)
        && bounds.min.x <= bounds.max.x && bounds.min.y <= bounds.max.y && bounds.min.z <= bounds.max.z;
}

/// @return 走査区間を絞れないときは全空間を覆う境界。
[[nodiscard]] AABB SweptQueryBounds(const math::Vector3& origin, const math::Vector3& unitDirection,
    float maxDistance, float radius);

bool RaycastInstance(const math::Vector3& origin, const math::Vector3& unitDirection, float maxDistance,
    const ColliderInstance& instance, float& hitDistance, math::Vector3& hitNormal);
bool SphereOverlapsInstance(const math::Vector3& center, float radius, const ColliderInstance& instance);
/// @pre instance.collider は有効。hitDistance は負値で初期化されていること。
void RaycastExpandedCollider(const math::Vector3& origin, const math::Vector3& unitDirection, float maxDistance,
    const ColliderInstance& instance, float expansionRadius, float& hitDistance, math::Vector3& hitNormal);

} /// @note namespace fbzz::physics::world_query_detail
