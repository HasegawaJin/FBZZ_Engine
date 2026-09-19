/// @file    Quaternion.hpp
/// @brief   クォータニオン回転 (ジンバルロック回避・Slerp補間)。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once

#include "Vector3.hpp"

namespace fbzz::math {

/// @note Quaternion.cpp が Matrix4.hpp を include するための前方宣言。
struct Matrix4;

struct Quaternion {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

    constexpr Quaternion() = default;
    constexpr Quaternion(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}

    static Quaternion Identity();
    static Quaternion FromAxisAngle(const Vector3& axis, float angleRad);
    static Quaternion FromEuler(const Vector3& eulerRad);
    static Quaternion LookRotation(const Vector3& forward,
                                   const Vector3& up = Vector3::UP);

    Quaternion  operator*(const Quaternion& rhs) const;
    Vector3     operator*(const Vector3& v)      const;
    Quaternion& operator*=(const Quaternion& rhs);
    bool        operator==(const Quaternion& rhs) const;

    float      Length()     const;
    Quaternion Normalized() const;
    Quaternion Conjugate()  const;
    Quaternion Inverse()    const;

    static Quaternion FromMatrix4(const Matrix4& m);

    /// @brief NLerp (正規化線形補間)。
    /// @note 等角速度ではないが Slerp より軽量。短弧補間には Slerp を使う。
    static Quaternion Lerp(const Quaternion& a, const Quaternion& b, float t);
    static Quaternion Slerp(const Quaternion& a, const Quaternion& b, float t);
    static float      Dot(const Quaternion& a, const Quaternion& b);
};

/// @note FBZZMath は DLL のため、.cpp に置くと呼び出しごとに DLL 境界を越えてインライン化されない。契約を報告しない小関数はヘッダーで定義する。
inline Quaternion Quaternion::Identity() { return {0.0f, 0.0f, 0.0f, 1.0f}; }

inline Quaternion Quaternion::operator*(const Quaternion& rhs) const {
    return {
        w * rhs.x + x * rhs.w + y * rhs.z - z * rhs.y,
        w * rhs.y - x * rhs.z + y * rhs.w + z * rhs.x,
        w * rhs.z + x * rhs.y - y * rhs.x + z * rhs.w,
        w * rhs.w - x * rhs.x - y * rhs.y - z * rhs.z
    };
}

/// @brief ベクトルへクォータニオン回転を適用する (q*v*q^-1)。
/// @note v + 2w(q×v) + 2(q×(q×v)) の展開形。フル四元数乗算より乗算回数が少ない。
inline Vector3 Quaternion::operator*(const Vector3& v) const {
    Vector3 qv  = {x, y, z};
    Vector3 t   = Vector3::Cross(qv, v) * 2.0f;
    return v + t * w + Vector3::Cross(qv, t);
}

inline Quaternion& Quaternion::operator*=(const Quaternion& rhs) {
    *this = *this * rhs;
    return *this;
}

inline bool Quaternion::operator==(const Quaternion& rhs) const {
    return NearlyEqual(x, rhs.x) && NearlyEqual(y, rhs.y)
        && NearlyEqual(z, rhs.z) && NearlyEqual(w, rhs.w);
}

inline float Quaternion::Length() const {
    return std::sqrt(x * x + y * y + z * z + w * w);
}

inline Quaternion Quaternion::Conjugate() const { return {-x, -y, -z, w}; }

inline float Quaternion::Dot(const Quaternion& a, const Quaternion& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

} // namespace fbzz::math
