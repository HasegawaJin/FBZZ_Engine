/// @file    Frustum.cpp
/// @brief   視錐台の平面抽出・交差判定実装。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include "Math/Frustum.hpp"

namespace fbzz::math {

// Gribb–Hartmann 法:
//   M * v 列ベクトル規則のとき clip.i = dot(row_i, world)
//   各平面の係数は「row_3 ± row_i」で求まる。DirectX 深度 [0,1] のため near = row_2 のみ。
Frustum Frustum::FromViewProjection(const Matrix4& vp)
{
    Frustum f;

    // left:   row3 + row0
    f.planes[0] = Plane{
        { vp.m[3][0] + vp.m[0][0], vp.m[3][1] + vp.m[0][1], vp.m[3][2] + vp.m[0][2] },
          vp.m[3][3] + vp.m[0][3]
    }.Normalized();

    // right:  row3 - row0
    f.planes[1] = Plane{
        { vp.m[3][0] - vp.m[0][0], vp.m[3][1] - vp.m[0][1], vp.m[3][2] - vp.m[0][2] },
          vp.m[3][3] - vp.m[0][3]
    }.Normalized();

    // bottom: row3 + row1
    f.planes[2] = Plane{
        { vp.m[3][0] + vp.m[1][0], vp.m[3][1] + vp.m[1][1], vp.m[3][2] + vp.m[1][2] },
          vp.m[3][3] + vp.m[1][3]
    }.Normalized();

    // top:    row3 - row1
    f.planes[3] = Plane{
        { vp.m[3][0] - vp.m[1][0], vp.m[3][1] - vp.m[1][1], vp.m[3][2] - vp.m[1][2] },
          vp.m[3][3] - vp.m[1][3]
    }.Normalized();

    // near:   row2  (DirectX 深度 [0,1]: clip.z >= 0)
    f.planes[4] = Plane{
        { vp.m[2][0], vp.m[2][1], vp.m[2][2] },
          vp.m[2][3]
    }.Normalized();

    // far:    row3 - row2
    f.planes[5] = Plane{
        { vp.m[3][0] - vp.m[2][0], vp.m[3][1] - vp.m[2][1], vp.m[3][2] - vp.m[2][2] },
          vp.m[3][3] - vp.m[2][3]
    }.Normalized();

    return f;
}

bool Frustum::Contains(const Vector3& point) const
{
    for (const Plane& p : planes)
        if (!p.IsOnPositiveSide(point)) return false;
    return true;
}

bool Frustum::IntersectsSphere(const Vector3& center, float radius) const
{
    for (const Plane& p : planes)
        if (p.SignedDistanceTo(center) < -radius) return false;
    return true;
}

bool Frustum::IntersectsAABB(const Vector3& center, const Vector3& halfExtents) const
{
    for (const Plane& p : planes) {
        // 各軸の half-extent を法線方向に投影した最大値 (符号付き絶対値の和)
        float r = halfExtents.x * (p.normal.x < 0.0f ? -p.normal.x : p.normal.x)
                + halfExtents.y * (p.normal.y < 0.0f ? -p.normal.y : p.normal.y)
                + halfExtents.z * (p.normal.z < 0.0f ? -p.normal.z : p.normal.z);
        if (p.SignedDistanceTo(center) < -r) return false;
    }
    return true;
}

} // namespace fbzz::math
