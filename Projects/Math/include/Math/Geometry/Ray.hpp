/// @file    Ray.hpp
/// @brief   レイ (origin + t * direction)。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#pragma once
#include <Math/Api.hpp>

#include "Math/LinearAlgebra/Vector3.hpp"
#include "Math/LinearAlgebra/Vector4.hpp"
#include "Math/LinearAlgebra/Matrix4.hpp"
#include "Plane.hpp"

namespace fbzz::math {

struct Ray {
    Vector3 origin; ///< レイの始点。交差対象と同じ座標系で表す。
    Vector3 direction; ///< レイが進む向き。球との交差判定では正規化済みであること。

    constexpr Ray() = default;
    constexpr Ray(const Vector3& o, const Vector3& d) : origin(o), direction(d) {}

    /// @brief t パラメータにおける点を返す。
    FBZZ_MATH_API Vector3 At(float t) const;

    /// @brief 平面との交差判定。
    /// @param[out] outT レイ始点から交点までのパラメータ。後方なら負、平行なら未変更。
    /// @return 平行、または交点がレイの後方 (t < 0) なら false。
    /// @note 法線の裏側から入る交差も成立する (片面判定はしない)。
    FBZZ_MATH_API bool IntersectPlane(const Plane& plane, float& outT) const;

    /// @brief 球との交差判定。
    /// @param[out] outT 前方の最近接交点までの距離。交差がなければ未変更。
    /// @pre direction は正規化済みであること。
    FBZZ_MATH_API bool IntersectSphere(const Vector3& center, float radius, float& outT) const;

    /// @brief 三角形との交差判定 (Möller-Trumbore アルゴリズム)。
    /// @param[out] outT レイ始点から交点までのパラメータ。交差がなければ未変更。
    FBZZ_MATH_API bool IntersectTriangle(const Vector3& vertexA, const Vector3& vertexB, const Vector3& vertexC,
                                        float& outT) const;

    /// @brief NDC 座標 (-1..1) と逆ビュープロジェクション行列からレイを生成する。
    FBZZ_MATH_API static Ray FromNDC(float ndcX, float ndcY,
                       const Vector3& cameraPos, const Matrix4& invViewProj);
};

} // namespace fbzz::math
