/// @file    BVHBounds.hpp
/// @brief   メッシュ BVH の構築・更新・検索で共有する境界計算。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#pragma once
#include <Physics/Collision/Shapes/Collider.hpp>
#include <algorithm>
#include <limits>

namespace fbzz::physics::mesh_bvh_detail
{
    inline AABB MakeEmptyBounds()
    {
        constexpr float HIGHEST_COORDINATE = std::numeric_limits<float>::max();
        constexpr float LOWEST_COORDINATE = std::numeric_limits<float>::lowest();
        return {{HIGHEST_COORDINATE, HIGHEST_COORDINATE, HIGHEST_COORDINATE},
                {LOWEST_COORDINATE, LOWEST_COORDINATE, LOWEST_COORDINATE}};
    }

    inline void IncludePoint(AABB& bounds, const math::Vector3& point)
    {
        bounds.min = {(std::min)(bounds.min.x, point.x), (std::min)(bounds.min.y, point.y),
                      (std::min)(bounds.min.z, point.z)};
        bounds.max = {(std::max)(bounds.max.x, point.x), (std::max)(bounds.max.y, point.y),
                      (std::max)(bounds.max.z, point.z)};
    }

    inline AABB TriangleBounds(const math::Vector3 (&vertices)[3])
    {
        AABB bounds{vertices[0], vertices[0]};
        IncludePoint(bounds, vertices[1]);
        IncludePoint(bounds, vertices[2]);
        return bounds;
    }
}
