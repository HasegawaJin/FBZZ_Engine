// FBZZ Engine
// Vector3.hpp | fbzz::math
// 3次元ベクトル (位置・方向・法線・RGB色)
#pragma once

namespace fbzz::math {

struct Vector3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vector3() = default;
    constexpr Vector3(float x, float y, float z) : x(x), y(y), z(z) {}

    Vector3  operator+(const Vector3& rhs) const;
    Vector3  operator-(const Vector3& rhs) const;
    Vector3  operator*(float s)            const;
    Vector3  operator/(float s)            const;
    Vector3  operator-()                   const;
    Vector3& operator+=(const Vector3& rhs);
    Vector3& operator-=(const Vector3& rhs);
    bool     operator==(const Vector3& rhs) const;
    bool     operator!=(const Vector3& rhs) const;

    float   Length()     const;
    float   LengthSq()   const;
    Vector3 Normalized() const;

    static float   Dot(const Vector3& a, const Vector3& b);
    static Vector3 Cross(const Vector3& a, const Vector3& b);
    static Vector3 Lerp(const Vector3& a, const Vector3& b, float t);

    static constexpr Vector3 ZERO    = {0.0f, 0.0f, 0.0f};
    static constexpr Vector3 ONE     = {1.0f, 1.0f, 1.0f};
    static constexpr Vector3 UP      = {0.0f, 1.0f, 0.0f};
    static constexpr Vector3 RIGHT   = {1.0f, 0.0f, 0.0f};
    static constexpr Vector3 FORWARD = {0.0f, 0.0f, 1.0f};
};

} // namespace fbzz::math
