/// @file    Collider.cpp
/// @brief   コライダー形状の定義 (Sphere / AABB / Capsule)。
/// @author  Hasegawa Jin
/// @date    2026-05-21
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

    /// 既定の体積は外接箱。基本形状は各派生クラスが厳密値で override する。
    float Collider::ComputeVolume() const
    {
        const AABB bounds = GetAABB();
        const math::Vector3 size = bounds.max - bounds.min;
        const float volume = size.x * size.y * size.z;
        return volume > 0.0f ? volume : 0.0f;
    }

} // namespace fbzz::physics
