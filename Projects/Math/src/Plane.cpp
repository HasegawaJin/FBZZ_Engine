/// @file    Plane.cpp
/// @brief   平面演算実装。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include "Math/Plane.hpp"
#include "Math/MathUtils.hpp"

namespace fbzz::math {

Plane Plane::Normalized() const
{
    float len = normal.Length();
    if (NearlyZero(len)) return *this;
    float invLen = 1.0f / len;
    return { normal * invLen, distance * invLen };
}

Plane Plane::FromPoints(const Vector3& p0, const Vector3& p1, const Vector3& p2)
{
    Vector3 n = Vector3::Cross(p1 - p0, p2 - p0).Normalized();
    return { n, -Vector3::Dot(n, p0) };
}

Plane Plane::FromNormalAndPoint(const Vector3& n, const Vector3& point)
{
    Vector3 nn = n.Normalized();
    return { nn, -Vector3::Dot(nn, point) };
}

} // namespace fbzz::math
