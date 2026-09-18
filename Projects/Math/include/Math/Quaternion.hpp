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

} // namespace fbzz::math
