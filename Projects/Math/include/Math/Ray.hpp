/// @file    Ray.hpp
/// @brief   レイ (origin + t * direction)。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#pragma once
#include <Math/Api.hpp>

#include "Vector3.hpp"
#include "Vector4.hpp"
#include "Matrix4.hpp"
#include "Plane.hpp"

namespace fbzz::math {

struct Ray {
    Vector3 origin;
    Vector3 direction; ///< @note 正規化済みであること。

    constexpr Ray() = default;
    constexpr Ray(const Vector3& o, const Vector3& d) : origin(o), direction(d) {}

    /// @brief t パラメータにおける点を返す。
    FBZZ_MATH_API Vector3 At(float t) const;

    /// @brief 平面との交差判定。
    /// @return 平行、または交点がレイの後方 (t < 0) なら false。
    /// @note 法線の裏側から入る交差も成立する (片面判定はしない)。
    FBZZ_MATH_API bool IntersectPlane(const Plane& plane, float& outT) const;

    /// @brief 球との交差判定。
    /// @pre direction は正規化済みであること。
    FBZZ_MATH_API bool IntersectSphere(const Vector3& center, float radius, float& outT) const;

    /// @brief 三角形との交差判定 (Möller-Trumbore アルゴリズム)。
    FBZZ_MATH_API bool IntersectTriangle(const Vector3& v0, const Vector3& v1, const Vector3& v2,
                           float& outT) const;

    /// @brief NDC 座標 (-1..1) と逆ビュープロジェクション行列からレイを生成する。
    FBZZ_MATH_API static Ray FromNDC(float ndcX, float ndcY,
                       const Vector3& cameraPos, const Matrix4& invViewProj);
};

} /// @note namespace fbzz::math
