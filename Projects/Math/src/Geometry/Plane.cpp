/// @file    Plane.cpp
/// @brief   平面演算実装。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include "Math/Geometry/Plane.hpp"
#include "Math/Scalar/MathUtils.hpp"

namespace fbzz::math {

Plane Plane::Normalized() const
{
    float len = normal.Length();
    if (NearlyZero(len)) {
        return *this;
    }
    // 平面を表す式全体を法線の長さで割ると、同じ平面のまま式の値を実距離として読める。
    float invLen = 1.0f / len;
    return { normal * invLen, distance * invLen };
}

Plane Plane::FromPoints(const Vector3& p0, const Vector3& p1, const Vector3& p2)
{
    Vector3 n = Vector3::Cross(p1 - p0, p2 - p0).Normalized();
    // 通過点 p0 を平面の式に入れると値は 0 になる。そのため定数項は内積の符号を反転した値になる。
    return { n, -Vector3::Dot(n, p0) };
}

Plane Plane::FromNormalAndPoint(const Vector3& n, const Vector3& point)
{
    Vector3 nn = n.Normalized();
    return { nn, -Vector3::Dot(nn, point) };
}

} /// @note namespace fbzz::math
