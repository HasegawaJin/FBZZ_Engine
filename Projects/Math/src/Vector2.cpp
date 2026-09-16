/// @file    Vector2.cpp
/// @brief   2次元ベクトルの演算実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Math/Vector2.hpp"
#include "Math/MathUtils.hpp"
#include <cmath>
#include "Math/MathContract.hpp"

namespace fbzz::math {

Vector2 Vector2::operator+(const Vector2& rhs) const { return {x + rhs.x, y + rhs.y}; }
Vector2 Vector2::operator-(const Vector2& rhs) const { return {x - rhs.x, y - rhs.y}; }
Vector2 Vector2::operator*(float s)            const { return {x * s, y * s}; }
Vector2 Vector2::operator/(float s)            const { return {x / s, y / s}; }

Vector2& Vector2::operator+=(const Vector2& rhs) { x += rhs.x; y += rhs.y; return *this; }
Vector2& Vector2::operator-=(const Vector2& rhs) { x -= rhs.x; y -= rhs.y; return *this; }

bool Vector2::operator==(const Vector2& rhs) const {
    return NearlyEqual(x, rhs.x) && NearlyEqual(y, rhs.y);
}
bool Vector2::operator!=(const Vector2& rhs) const { return !(*this == rhs); }

float   Vector2::LengthSq()   const { return x * x + y * y; }
float   Vector2::Length()     const { return std::sqrt(LengthSq()); }
Vector2 Vector2::Normalized() const {
    float len = Length();
    FBZZ_MATH_CONTRACT(!NearlyZero(len),
                       "zero-length vector normalized; returning (0,0)");
    if (NearlyZero(len)) return ZERO;
    return *this / len;
}

float Vector2::Dot(const Vector2& a, const Vector2& b) {
    return a.x * b.x + a.y * b.y;
}

Vector2 Vector2::Lerp(const Vector2& a, const Vector2& b, float t) {
    return a + (b - a) * t;
}

} // namespace fbzz::math
