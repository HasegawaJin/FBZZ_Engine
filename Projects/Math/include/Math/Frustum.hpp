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

} // namespace fbzz::math
