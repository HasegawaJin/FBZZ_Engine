/// @file    Vector4.cpp
/// @brief   4次元ベクトルの演算実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Math/Vector4.hpp"
#include "Math/MathUtils.hpp"
#include <cmath>
#include "Math/MathContract.hpp"

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
    FBZZ_MATH_CONTRACT(!NearlyZero(len),
                       "zero-length vector normalized; returning (0,0,0,0)");
    if (NearlyZero(len)) return ZERO;
    return *this * (1.0f / len);
}

Vector3 Vector4::XYZ() const { return {x, y, z}; }

} // namespace fbzz::math
