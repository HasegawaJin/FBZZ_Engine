// FBZZ Engine
// Vector2.hpp | fbzz::math
// 2次元ベクトル (UV座標・スクリーン座標)
#pragma once

namespace fbzz::math {

struct Vector2 {
    float x = 0.0f, y = 0.0f;

    constexpr Vector2() = default;
    constexpr Vector2(float x, float y) : x(x), y(y) {}

    Vector2  operator+(const Vector2& rhs) const;
    Vector2  operator-(const Vector2& rhs) const;
    Vector2  operator*(float s)            const;
    Vector2  operator/(float s)            const;
    Vector2& operator+=(const Vector2& rhs);
    Vector2& operator-=(const Vector2& rhs);
    bool     operator==(const Vector2& rhs) const;
    bool     operator!=(const Vector2& rhs) const;

    float   Length()     const;
    float   LengthSq()   const;
    Vector2 Normalized() const;

    static float   Dot(const Vector2& a, const Vector2& b);
    static Vector2 Lerp(const Vector2& a, const Vector2& b, float t);

    static const Vector2 ZERO;
    static const Vector2 ONE;
};

inline const Vector2 Vector2::ZERO = {0.0f, 0.0f};
inline const Vector2 Vector2::ONE  = {1.0f, 1.0f};

} // namespace fbzz::math
