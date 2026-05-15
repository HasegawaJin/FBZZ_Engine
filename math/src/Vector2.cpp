// FBZZ Engine
// Vector2.cpp | fbzz::math
// 2次元ベクトルの演算実装
#include "math/Vector2.hpp"
#include "math/MathUtils.hpp"
#include <cmath>
#include <cassert>

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
    assert(!NearlyZero(len) && "Cannot normalize a zero-length vector");
    return *this / len;
}

float Vector2::Dot(const Vector2& a, const Vector2& b) {
    return a.x * b.x + a.y * b.y;
}

Vector2 Vector2::Lerp(const Vector2& a, const Vector2& b, float t) {
    return a + (b - a) * t;
}

} // namespace fbzz::math
