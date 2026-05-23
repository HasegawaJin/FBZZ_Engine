// FBZZ Engine
// Ray.hpp | fbzz::math
// レイ (origin + t * direction)
#pragma once

#include "Vector3.hpp"
#include "Vector4.hpp"
#include "Matrix4.hpp"
#include "Plane.hpp"

namespace fbzz::math {

struct Ray {
    Vector3 origin;
    Vector3 direction; // 正規化済みであることを前提とする

    constexpr Ray() = default;
    constexpr Ray(const Vector3& o, const Vector3& d) : origin(o), direction(d) {}

    // t パラメータの点を返す
    Vector3 At(float t) const;

    // 平面との交差。outT < 0 は後方交差。平行なら false を返す
    bool IntersectPlane(const Plane& plane, float& outT) const;

    // 球との交差 (direction は正規化済み前提)
    bool IntersectSphere(const Vector3& center, float radius, float& outT) const;

    // 三角形との交差 (Möller–Trumbore アルゴリズム)
    bool IntersectTriangle(const Vector3& v0, const Vector3& v1, const Vector3& v2,
                           float& outT) const;

    // NDC座標 (-1~+1) と逆ビュープロジェクション行列からレイを生成
    static Ray FromNDC(float ndcX, float ndcY,
                       const Vector3& cameraPos, const Matrix4& invViewProj);
};

} // namespace fbzz::math
