// FBZZ Engine
// Plane.cpp | fbzz::math
// 平面演算実装
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

float Plane::SignedDistanceTo(const Vector3& point) const
{
    return Vector3::Dot(normal, point) + distance;
}

bool Plane::IsOnPositiveSide(const Vector3& point) const
{
    return SignedDistanceTo(point) >= 0.0f;
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
