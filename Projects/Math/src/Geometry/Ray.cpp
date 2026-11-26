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
    // 内積はレイが 1 進む間に平面の法線方向へ進む量。ほぼ 0 なら平行で、交点を一つに決められない。
    const float normalDirection = Vector3::Dot(plane.normal, direction);
    if (NearlyZero(normalDirection)) {
        return false;
    }

    // 平行でなければ、後方であっても平面に達するパラメータを返す。
    outT = -(Vector3::Dot(plane.normal, origin) + plane.distance) / normalDirection;
    return outT >= 0.0f;
}

bool Ray::IntersectSphere(const Vector3& center, float radius, float& outT) const
{
    // direction は正規化済みのため二次方程式の a = 1、半 b 形式で計算する。
    const Vector3 originFromCenter = origin - center;
    const float halfLinearCoefficient = Vector3::Dot(originFromCenter, direction);
    const float constantCoefficient = Vector3::Dot(originFromCenter, originFromCenter) - radius * radius;

    // 半 b 形式の判別式が負なら球面に届かない。
    const float discriminant = halfLinearCoefficient * halfLinearCoefficient - constantCoefficient;
    if (discriminant < 0.0f) {
        return false;
    }

    // 入口が前方なら最近点になる。レイが球内から始まる場合は前方にある出口を採る。
    const float root = std::sqrt(discriminant);
    const float entry = -halfLinearCoefficient - root;
    const float exit = -halfLinearCoefficient + root;
    if (entry > EPSILON) {
        outT = entry;
        return true;
    }
    if (exit > EPSILON) {
        outT = exit;
        return true;
    }
    return false;
}

bool Ray::IntersectTriangle(const Vector3& vertexA, const Vector3& vertexB, const Vector3& vertexC,
                            float& outT) const
{
    // Möller–Trumbore 法で、レイが三角形の面と平行な場合を除く。
    const Vector3 edgeAB = vertexB - vertexA;
    const Vector3 edgeAC = vertexC - vertexA;
    const Vector3 crossDirectionWithAC = Vector3::Cross(direction, edgeAC);
    const float determinant = Vector3::Dot(edgeAB, crossDirectionWithAC);
    const bool parallelToFace = determinant > -EPSILON && determinant < EPSILON;
    if (parallelToFace) {
        return false;
    }
    const float inverseDeterminant = 1.0f / determinant;

    // B と C の重みを順に求める。残りの A の重みも 0 以上なら三角形の内側にある。
    const Vector3 originFromA = origin - vertexA;
    const float weightB = Vector3::Dot(originFromA, crossDirectionWithAC) * inverseDeterminant;
    if (weightB < 0.0f || weightB > 1.0f) {
        return false;
    }
    const Vector3 crossOriginWithAB = Vector3::Cross(originFromA, edgeAB);
    const float weightC = Vector3::Dot(direction, crossOriginWithAB) * inverseDeterminant;
    if (weightC < 0.0f || (weightB + weightC) > 1.0f) {
        return false;
    }

    // 三角形の内側でも、レイの後方や始点に近すぎる交点は採らない。
    const float t = Vector3::Dot(edgeAC, crossOriginWithAB) * inverseDeterminant;
    if (t < EPSILON) {
        return false;
    }
    outT = t;
    return true;
}

Ray Ray::FromNDC(float ndcX, float ndcY,
                 const Vector3& cameraPos, const Matrix4& invViewProj)
{
    Vector4 nearClip = { ndcX, ndcY, 0.0f, 1.0f };
    Vector4 farClip  = { ndcX, ndcY, 1.0f, 1.0f };

    // 画面上の同じ点を手前と奥の 2 点に戻す。それぞれ w で割ってから差を取ると、世界空間の視線の向きが得られる。
    Vector4 nearWorld = invViewProj * nearClip;
    Vector4 farWorld  = invViewProj * farClip;
    if (!NearlyZero(nearWorld.w)) {
        nearWorld = nearWorld * (1.0f / nearWorld.w);
    }
    if (!NearlyZero(farWorld.w)) {
        farWorld  = farWorld  * (1.0f / farWorld.w);
    }

    const Vector3 nearPoint = {nearWorld.x, nearWorld.y, nearWorld.z};
    const Vector3 farPoint = {farWorld.x, farWorld.y, farWorld.z};
    return {cameraPos, (farPoint - nearPoint).Normalized()};
}

} // namespace fbzz::math
