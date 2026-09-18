/// @file    Frustum.cpp
/// @brief   視錐台の平面抽出・交差判定実装。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include "Math/Frustum.hpp"

namespace fbzz::math {

/// @note Gribb-Hartmann 法。M * v (列ベクトル規則) では clip.i = dot(row_i, world)。
/// @note 各平面の係数は row_3 ± row_i で求まる。DirectX 深度 [0,1] のため near は row_2 のみ。
Frustum Frustum::FromViewProjection(const Matrix4& vp)
{
    Frustum f;

    /// @note left: row3 + row0
    f.SetPlane(0, Plane{
        { vp.m[3][0] + vp.m[0][0], vp.m[3][1] + vp.m[0][1], vp.m[3][2] + vp.m[0][2] },
          vp.m[3][3] + vp.m[0][3]
    }.Normalized());

    /// @note right: row3 - row0
    f.SetPlane(1, Plane{
        { vp.m[3][0] - vp.m[0][0], vp.m[3][1] - vp.m[0][1], vp.m[3][2] - vp.m[0][2] },
          vp.m[3][3] - vp.m[0][3]
    }.Normalized());

    /// @note bottom: row3 + row1
    f.SetPlane(2, Plane{
        { vp.m[3][0] + vp.m[1][0], vp.m[3][1] + vp.m[1][1], vp.m[3][2] + vp.m[1][2] },
          vp.m[3][3] + vp.m[1][3]
    }.Normalized());

    /// @note top: row3 - row1
    f.SetPlane(3, Plane{
        { vp.m[3][0] - vp.m[1][0], vp.m[3][1] - vp.m[1][1], vp.m[3][2] - vp.m[1][2] },
          vp.m[3][3] - vp.m[1][3]
    }.Normalized());

    /// @note near: row2 (DirectX 深度 [0,1]: clip.z >= 0)
    f.SetPlane(4, Plane{
        { vp.m[2][0], vp.m[2][1], vp.m[2][2] },
          vp.m[2][3]
    }.Normalized());

    /// @note far: row3 - row2
    f.SetPlane(5, Plane{
        { vp.m[3][0] - vp.m[2][0], vp.m[3][1] - vp.m[2][1], vp.m[3][2] - vp.m[2][2] },
          vp.m[3][3] - vp.m[2][3]
    }.Normalized());

    return f;
}

} // namespace fbzz::math
