// FBZZ Engine
// Frustum.hpp | fbzz::math
// 視錐台 (6平面による凸包)
#pragma once

#include "Plane.hpp"
#include "Vector3.hpp"
#include "Matrix4.hpp"

// Plane.hpp include 後に別ヘッダ経由で Plane マクロが再定義された場合に備える。
// WHAT: Frustum は Plane 型を配列で保持するため、ここで名前を必ず型として解決させる。
#ifdef Plane
#undef Plane
#endif

namespace fbzz::math {

struct Frustum {
    // planes[0]=left, [1]=right, [2]=bottom, [3]=top, [4]=near, [5]=far
    Plane planes[6];

    // 点が視錐台内にあるか
    bool Contains(const Vector3& point) const;

    // 球が視錐台と交差するか (完全に外側なら false)
    bool IntersectsSphere(const Vector3& center, float radius) const;

    // AABB が視錐台と交差するか (完全に外側なら false)
    bool IntersectsAABB(const Vector3& center, const Vector3& halfExtents) const;

    // ビュープロジェクション行列から6平面を抽出 (Gribb–Hartmann 法)
    // DirectX 左手系・深度 [0,1]・列ベクトル規則 (M * v) を前提とする
    static Frustum FromViewProjection(const Matrix4& vp);
};

} // namespace fbzz::math
