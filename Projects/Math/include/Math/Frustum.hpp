/// @file    Frustum.hpp
/// @brief   視錐台 (6平面による凸包)。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#pragma once

#include "Plane.hpp"
#include "Vector3.hpp"
#include "Matrix4.hpp"

/// @note Plane.hpp include 後に別ヘッダ経由で Plane マクロが再定義された場合に備える。
///       Frustum は Plane 型を配列で保持するため、ここで名前を必ず型として解決させる。
#ifdef Plane
#undef Plane
#endif

namespace fbzz::math {

struct Frustum {
    /// @brief 6 平面 (left/right/bottom/top/near/far の順)。
    Plane planes[6];

    /// @brief 点が視錐台内にあるかを判定する。
    bool Contains(const Vector3& point) const;

    /// @brief 球が視錐台と交差するかを判定する。
    /// @return 完全に外側なら false。
    bool IntersectsSphere(const Vector3& center, float radius) const;

    /// @brief AABB が視錐台と交差するかを判定する。
    /// @return 完全に外側なら false。
    bool IntersectsAABB(const Vector3& center, const Vector3& halfExtents) const;

    /// @brief ビュープロジェクション行列から 6 平面を抽出する (Gribb-Hartmann 法)。
    /// @pre DirectX 左手系、深度 [0,1]、列ベクトル規則 (M * v)。
    static Frustum FromViewProjection(const Matrix4& vp);
};

/// @note FBZZMath は DLL のため、.cpp に置くと呼び出しごとに DLL 境界を越えてインライン化されない。カリングで物体ごとに呼ぶためヘッダーで定義する。
inline bool Frustum::Contains(const Vector3& point) const
{
    for (const Plane& p : planes)
        if (!p.IsOnPositiveSide(point)) return false;
    return true;
}

inline bool Frustum::IntersectsSphere(const Vector3& center, float radius) const
{
    for (const Plane& p : planes)
        if (p.SignedDistanceTo(center) < -radius) return false;
    return true;
}

inline bool Frustum::IntersectsAABB(const Vector3& center, const Vector3& halfExtents) const
{
    for (const Plane& p : planes) {
        /// @note 各軸の half-extent を法線方向に投影した最大値 (符号付き絶対値の和)。
        float r = halfExtents.x * (p.normal.x < 0.0f ? -p.normal.x : p.normal.x)
                + halfExtents.y * (p.normal.y < 0.0f ? -p.normal.y : p.normal.y)
                + halfExtents.z * (p.normal.z < 0.0f ? -p.normal.z : p.normal.z);
        if (p.SignedDistanceTo(center) < -r) return false;
    }
    return true;
}

} // namespace fbzz::math
