/// @file    Segment.hpp
/// @brief   点と有限線分の最近接点・距離を求める。
/// @author  Hasegawa Jin
/// @date    2026-09-13
#pragma once
#include <Math/MathUtils.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::math {

/// @brief 線分上で点に最も近い点を返す。
/// @pre 入力は同一座標系の有限値。
/// @note 長さが EPSILON 以下の線分は from に縮退する。
[[nodiscard]] inline Vector3 ClosestPointOnSegment(const Vector3& point,
    const Vector3& from, const Vector3& to)
{
    const Vector3 segment = to - from;
    const float lengthSq = segment.LengthSq();
    if (lengthSq <= EPSILON * EPSILON) return from;
    return from + segment * Clamp01(Vector3::Dot(point - from, segment) / lengthSq);
}

/// @brief 点と線分の距離を求める。
/// @param outAlong from から最近接点までの線分上の距離。単位は入力座標と同じ。
/// @return 点と最近接点の距離。
[[nodiscard]] inline float DistanceToSegment(const Vector3& point,
    const Vector3& from, const Vector3& to, float& outAlong)
{
    const Vector3 closest = ClosestPointOnSegment(point, from, to);
    outAlong = (closest - from).Length();
    return (point - closest).Length();
}

} // namespace fbzz::math
