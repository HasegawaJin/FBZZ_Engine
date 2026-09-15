/// @file    GroundFireArea.hpp
/// @brief   足元の移動線分と地面の燃焼円柱の交差判定。
/// @author  Hasegawa Jin
/// @date    2026-09-15
#pragma once
#include <Math/Segment.hpp>
#include <algorithm>
#include <cmath>

namespace sandbox {

struct GroundFireArea {
    static bool Intersects(const fbzz::math::Vector3& from, const fbzz::math::Vector3& to,
                           const fbzz::math::Vector3& center, float radius, float height)
    {
        if (radius <= 0.0f || height <= 0.0f) return false;
        auto a = from - center;
        auto b = to - center;
        const float dy = b.y - a.y;
        float enter = 0.0f, leave = 1.0f;
        if (std::abs(dy) < 0.00001f) {
            if (a.y < -0.3f || a.y > height) return false;
        } else {
            float low = (-0.3f - a.y) / dy;
            float high = (height - a.y) / dy;
            if (low > high) std::swap(low, high);
            enter = std::max(enter, low);
            leave = std::min(leave, high);
            if (enter > leave) return false;
        }
        const auto step = b - a;
        b = a + step * leave;
        a += step * enter;
        a.y = b.y = 0.0f;
        return fbzz::math::ClosestPointOnSegment({}, a, b).LengthSq() <= radius * radius;
    }
};

} // namespace sandbox
