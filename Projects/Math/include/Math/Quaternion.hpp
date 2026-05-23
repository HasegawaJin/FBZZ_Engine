// FBZZ Engine
// Quaternion.hpp | fbzz::math
// クォータニオン回転 (ジンバルロック回避・Slerp補間)
#pragma once

#include "Vector3.hpp"

namespace fbzz::math {

struct Matrix4; // forward declaration — Quaternion.cpp includes Matrix4.hpp

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

    static Quaternion Lerp(const Quaternion& a, const Quaternion& b, float t);
    static Quaternion Slerp(const Quaternion& a, const Quaternion& b, float t);
    static float      Dot(const Quaternion& a, const Quaternion& b);
};

} // namespace fbzz::math
