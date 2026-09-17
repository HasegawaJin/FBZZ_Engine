/// @file    Ray.cpp
/// @brief   レイの交差判定実装。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include "Math/Ray.hpp"
#include "Math/MathUtils.hpp"
#include <cmath>

namespace fbzz::math {

Vector3 Ray::At(float t) const
{
    return origin + direction * t;
}

bool Ray::IntersectPlane(const Plane& plane, float& outT) const
{
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
    float   disc = b * b - c;
    if (disc < 0.0f) return false;
    float s  = std::sqrt(disc);
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

    Vector4 nearWorld = invViewProj * nearClip;
    Vector4 farWorld  = invViewProj * farClip;
    if (!NearlyZero(nearWorld.w)) nearWorld = nearWorld * (1.0f / nearWorld.w);
    if (!NearlyZero(farWorld.w))  farWorld  = farWorld  * (1.0f / farWorld.w);

    Vector3 nearW = { nearWorld.x, nearWorld.y, nearWorld.z };
    Vector3 farW  = { farWorld.x,  farWorld.y,  farWorld.z  };
    return { cameraPos, (farW - nearW).Normalized() };
}

} // namespace fbzz::math
