/// @file    ScalarReference.hpp
/// @brief   Math の SIMD 化前のスカラー実装の写し。SIMD 版と突き合わせる参照実装。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @note 製品コードに 2 経路を持たないため、スカラー版はテスト側だけに置く。
/// @see Docs/design/math-simd.md §5 正しさの確かめ方
#pragma once

#include <Math/Frustum.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Plane.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

#include <array>
#include <cmath>

namespace fbzz::tests::reference {

using Planes = std::array<math::Plane, 6>;

inline math::Matrix4 Multiply(const math::Matrix4& a, const math::Matrix4& b)
{
    math::Matrix4 result;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k)
                result.m[r][c] += a.m[r][k] * b.m[k][c];
    return result;
}

inline math::Vector4 Multiply(const math::Matrix4& a, const math::Vector4& v)
{
    return {
        a.m[0][0]*v.x + a.m[0][1]*v.y + a.m[0][2]*v.z + a.m[0][3]*v.w,
        a.m[1][0]*v.x + a.m[1][1]*v.y + a.m[1][2]*v.z + a.m[1][3]*v.w,
        a.m[2][0]*v.x + a.m[2][1]*v.y + a.m[2][2]*v.z + a.m[2][3]*v.w,
        a.m[3][0]*v.x + a.m[3][1]*v.y + a.m[3][2]*v.z + a.m[3][3]*v.w
    };
}

inline math::Matrix4 Transpose(const math::Matrix4& a)
{
    math::Matrix4 result;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            result.m[r][c] = a.m[c][r];
    return result;
}

/// @return 特異 (行列式がほぼ 0) なら単位行列。製品版の契約と同じ。
inline math::Matrix4 Inverse(const math::Matrix4& mat)
{
    const float* a = &mat.m[0][0];

    const float b00 = a[ 0]*a[ 5] - a[ 1]*a[ 4];
    const float b01 = a[ 0]*a[ 6] - a[ 2]*a[ 4];
    const float b02 = a[ 0]*a[ 7] - a[ 3]*a[ 4];
    const float b03 = a[ 1]*a[ 6] - a[ 2]*a[ 5];
    const float b04 = a[ 1]*a[ 7] - a[ 3]*a[ 5];
    const float b05 = a[ 2]*a[ 7] - a[ 3]*a[ 6];
    const float b06 = a[ 8]*a[13] - a[ 9]*a[12];
    const float b07 = a[ 8]*a[14] - a[10]*a[12];
    const float b08 = a[ 8]*a[15] - a[11]*a[12];
    const float b09 = a[ 9]*a[14] - a[10]*a[13];
    const float b10 = a[ 9]*a[15] - a[11]*a[13];
    const float b11 = a[10]*a[15] - a[11]*a[14];

    const float det = b00*b11 - b01*b10 + b02*b09 + b03*b08 - b04*b07 + b05*b06;
    if (math::NearlyZero(det)) return math::Matrix4::Identity();

    const float inv = 1.0f / det;
    math::Matrix4 result;
    float* r = &result.m[0][0];
    r[ 0] = ( a[ 5]*b11 - a[ 6]*b10 + a[ 7]*b09) * inv;
    r[ 1] = (-a[ 1]*b11 + a[ 2]*b10 - a[ 3]*b09) * inv;
    r[ 2] = ( a[13]*b05 - a[14]*b04 + a[15]*b03) * inv;
    r[ 3] = (-a[ 9]*b05 + a[10]*b04 - a[11]*b03) * inv;
    r[ 4] = (-a[ 4]*b11 + a[ 6]*b08 - a[ 7]*b07) * inv;
    r[ 5] = ( a[ 0]*b11 - a[ 2]*b08 + a[ 3]*b07) * inv;
    r[ 6] = (-a[12]*b05 + a[14]*b02 - a[15]*b01) * inv;
    r[ 7] = ( a[ 8]*b05 - a[10]*b02 + a[11]*b01) * inv;
    r[ 8] = ( a[ 4]*b10 - a[ 5]*b08 + a[ 7]*b06) * inv;
    r[ 9] = (-a[ 0]*b10 + a[ 1]*b08 - a[ 3]*b06) * inv;
    r[10] = ( a[12]*b04 - a[13]*b02 + a[15]*b00) * inv;
    r[11] = (-a[ 8]*b04 + a[ 9]*b02 - a[11]*b00) * inv;
    r[12] = (-a[ 4]*b09 + a[ 5]*b07 - a[ 6]*b06) * inv;
    r[13] = ( a[ 0]*b09 - a[ 1]*b07 + a[ 2]*b06) * inv;
    r[14] = (-a[12]*b03 + a[13]*b01 - a[14]*b00) * inv;
    r[15] = ( a[ 8]*b03 - a[ 9]*b01 + a[10]*b00) * inv;
    return result;
}

/// @pre q は正規化済みでなくてよい (製品版と同じく内部で正規化する)。
inline math::Matrix4 Rotate(const math::Quaternion& q)
{
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    const math::Quaternion qn{ q.x / len, q.y / len, q.z / len, q.w / len };
    const float xx = qn.x * qn.x, yy = qn.y * qn.y, zz = qn.z * qn.z;
    const float xy = qn.x * qn.y, xz = qn.x * qn.z, yz = qn.y * qn.z;
    const float wx = qn.w * qn.x, wy = qn.w * qn.y, wz = qn.w * qn.z;

    math::Matrix4 result = math::Matrix4::Identity();
    result.m[0][0] = 1.0f - 2.0f * (yy + zz);
    result.m[0][1] = 2.0f * (xy - wz);
    result.m[0][2] = 2.0f * (xz + wy);
    result.m[1][0] = 2.0f * (xy + wz);
    result.m[1][1] = 1.0f - 2.0f * (xx + zz);
    result.m[1][2] = 2.0f * (yz - wx);
    result.m[2][0] = 2.0f * (xz - wy);
    result.m[2][1] = 2.0f * (yz + wx);
    result.m[2][2] = 1.0f - 2.0f * (xx + yy);
    return result;
}

/// @brief 定義どおり T * R * S の積で組む。
inline math::Matrix4 Trs(const math::Vector3& t, const math::Quaternion& r, const math::Vector3& s)
{
    math::Matrix4 translate = math::Matrix4::Identity();
    translate.m[0][3] = t.x;
    translate.m[1][3] = t.y;
    translate.m[2][3] = t.z;
    math::Matrix4 scale = math::Matrix4::Identity();
    scale.m[0][0] = s.x;
    scale.m[1][1] = s.y;
    scale.m[2][2] = s.z;
    return Multiply(Multiply(translate, Rotate(r)), scale);
}

inline Planes PlanesOf(const math::Frustum& frustum)
{
    Planes planes;
    for (int i = 0; i < 6; ++i) planes[i] = frustum.planes[i];
    return planes;
}

/// @return 6 平面それぞれの «内側へのはみ出し» (符号付き距離 + 半径) の最小値。負なら完全に外側。
/// @note 判定が境界ぎりぎりの入力を突き合わせから外すために、真偽ではなく余裕を返す。
inline float SphereMargin(const Planes& planes, const math::Vector3& center, float radius)
{
    float margin = INFINITY;
    for (const math::Plane& p : planes)
        margin = std::fmin(margin, math::Vector3::Dot(p.normal, center) + p.distance + radius);
    return margin;
}

/// @return SphereMargin と同じく、AABB を法線へ投影した半径で測った余裕の最小値。
inline float AabbMargin(const Planes& planes, const math::Vector3& center, const math::Vector3& halfExtents)
{
    float margin = INFINITY;
    for (const math::Plane& p : planes) {
        const float r = halfExtents.x * std::fabs(p.normal.x)
                      + halfExtents.y * std::fabs(p.normal.y)
                      + halfExtents.z * std::fabs(p.normal.z);
        margin = std::fmin(margin, math::Vector3::Dot(p.normal, center) + p.distance + r);
    }
    return margin;
}

} // namespace fbzz::tests::reference
