// FBZZ Engine
// Vector4.cpp | fbzz::math
// 4次元ベクトルの演算実装
#include "Math/Vector4.hpp"
#include "Math/MathUtils.hpp"
#include <cmath>
#include <cassert>

namespace fbzz::math {

Vector4 Vector4::operator+(const Vector4& rhs) const { return {x + rhs.x, y + rhs.y, z + rhs.z, w + rhs.w}; }
Vector4 Vector4::operator-(const Vector4& rhs) const { return {x - rhs.x, y - rhs.y, z - rhs.z, w - rhs.w}; }
Vector4 Vector4::operator*(float s)            const { return {x * s, y * s, z * s, w * s}; }

bool Vector4::operator==(const Vector4& rhs) const {
    return NearlyEqual(x, rhs.x) && NearlyEqual(y, rhs.y)
        && NearlyEqual(z, rhs.z) && NearlyEqual(w, rhs.w);
}

float Vector4::Length() const {
    return std::sqrt(x * x + y * y + z * z + w * w);
}

Vector4 Vector4::Normalized() const {
    float len = Length();
    assert(!NearlyZero(len) && "Cannot normalize a zero-length vector");
    return *this * (1.0f / len);
}

Vector3 Vector4::XYZ() const { return {x, y, z}; }

} // namespace fbzz::math
