// FBZZ Engine
// Collider.cpp | fbzz::physics
// コライダー形状の定義 (Sphere / AABB / Capsule)
#include <Physics/Collider.hpp>
#include <algorithm>

namespace fbzz::physics
{

    bool AABB::Overlaps(const AABB& other) const
    {
        return (min.x <= other.max.x && max.x >= other.min.x) &&
            (min.y <= other.max.y && max.y >= other.min.y) &&
            (min.z <= other.max.z && max.z >= other.min.z);
    }

    AABB AABB::Merge(const AABB& other) const
    {
        return {
            { std::min(min.x, other.min.x), std::min(min.y, other.min.y), std::min(min.z, other.min.z) },
            { std::max(max.x, other.max.x), std::max(max.y, other.max.y), std::max(max.z, other.max.z) }
        };
    }

} // namespace fbzz::physics
