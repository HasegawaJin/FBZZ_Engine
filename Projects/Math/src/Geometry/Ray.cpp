/// @file    Ray.cpp
/// @brief   レイの交差判定実装。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include "Math/Geometry/Ray.hpp"
#include "Math/Scalar/MathUtils.hpp"
#include <cmath>

namespace fbzz::math {

Vector3 Ray::At(float t) const
{
    return origin + direction * t;
}

bool Ray::IntersectPlane(const Plane& plane, float& outT) const
{
    /// @note 内積はレイが 1 進む間に平面の法線方向へ進む量。ほぼ 0 なら平行で、交点を一つに決められない。
    float denom = Vector3::Dot(plane.normal, direction);
    if (NearlyZero(denom)) return false;
    outT = -(Vector3::Dot(plane.normal, origin) + plane.distance) / denom;
    return outT >= 0.0f;
}

bool Ray::IntersectSphere(const Vector3& center, float radius, float& outT) const
{
    /// @note direction は正規化済みのため二次方程式の a = 1、半 b 形式で計算する。
    Vector3 oc   = origin - center;
    float   b    = Vector3::Dot(oc, direction);
    float   c    = Vector3::Dot(oc, oc) - radius * radius;
    /// @note レイ上の点が球面にある条件は t² + 2bt + c = 0。判別式が負ならレイは球面に届かない。
    float   disc = b * b - c;
    if (disc < 0.0f) return false;
    float s  = std::sqrt(disc);
    /// @note 小さい解は球への入口、大きい解は出口。始点が球内なら入口は後方なので、前方の出口を採る。
    float t0 = -b - s;
    float t1 = -b + s;
    float t  = (t0 > EPSILON) ? t0 : ((t1 > EPSILON) ? t1 : -1.0f);
    if (t < EPSILON) return false;
    outT = t;
    return true;
}

bool Ray::IntersectTriangle(const Vector3& v0, const Vector3& v1, const Vector3& v2,
                            float& outT) const
{
    /// @note Möller-Trumbore アルゴリズム。
    Vector3 e1  = v1 - v0;
    Vector3 e2  = v2 - v0;
    Vector3 p   = Vector3::Cross(direction, e2);
    float   det = Vector3::Dot(e1, p);
    if (det > -EPSILON && det < EPSILON) return false;
    float invDet = 1.0f / det;

    Vector3 tvec = origin - v0;
    /// @note u と v は三角形の 2 辺に沿う割合。残りの頂点の割合は 1 − u − v で、三つとも 0 以上なら三角形の内側にある。
    float   u    = Vector3::Dot(tvec, p) * invDet;
    if (u < 0.0f || u > 1.0f) return false;

    Vector3 qvec = Vector3::Cross(tvec, e1);
    float   v    = Vector3::Dot(direction, qvec) * invDet;
    if (v < 0.0f || (u + v) > 1.0f) return false;

    float t = Vector3::Dot(e2, qvec) * invDet;
    if (t < EPSILON) return false;
    outT = t;
    return true;
}

Ray Ray::FromNDC(float ndcX, float ndcY,
                 const Vector3& cameraPos, const Matrix4& invViewProj)
{
    Vector4 nearClip = { ndcX, ndcY, 0.0f, 1.0f };
    Vector4 farClip  = { ndcX, ndcY, 1.0f, 1.0f };

    /// @note 画面上の同じ点を手前と奥の 2 点に戻す。それぞれ w で割ってから差を取ると、世界空間の視線の向きが得られる。
    Vector4 nearWorld = invViewProj * nearClip;
    Vector4 farWorld  = invViewProj * farClip;
    if (!NearlyZero(nearWorld.w)) nearWorld = nearWorld * (1.0f / nearWorld.w);
    if (!NearlyZero(farWorld.w))  farWorld  = farWorld  * (1.0f / farWorld.w);

    Vector3 nearW = { nearWorld.x, nearWorld.y, nearWorld.z };
    Vector3 farW  = { farWorld.x,  farWorld.y,  farWorld.z  };
    return { cameraPos, (farW - nearW).Normalized() };
}

} /// @note namespace fbzz::math
