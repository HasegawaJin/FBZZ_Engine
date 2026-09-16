/// @file    Vector3.cpp
/// @brief   3次元ベクトルの演算実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Math/Vector3.hpp"
#include "Math/MathUtils.hpp"
#include <cmath>
#include "Math/MathContract.hpp"

namespace fbzz::math {

Vector3 Vector3::operator+(const Vector3& rhs) const { return {x + rhs.x, y + rhs.y, z + rhs.z}; }
Vector3 Vector3::operator-(const Vector3& rhs) const { return {x - rhs.x, y - rhs.y, z - rhs.z}; }
Vector3 Vector3::operator*(float s)            const { return {x * s, y * s, z * s}; }
Vector3 Vector3::operator/(float s)            const { return {x / s, y / s, z / s}; }
Vector3 Vector3::operator-()                   const { return {-x, -y, -z}; }

Vector3& Vector3::operator+=(const Vector3& rhs) { x += rhs.x; y += rhs.y; z += rhs.z; return *this; }
Vector3& Vector3::operator-=(const Vector3& rhs) { x -= rhs.x; y -= rhs.y; z -= rhs.z; return *this; }

bool Vector3::operator==(const Vector3& rhs) const {
    return NearlyEqual(x, rhs.x) && NearlyEqual(y, rhs.y) && NearlyEqual(z, rhs.z);
}
bool Vector3::operator!=(const Vector3& rhs) const { return !(*this == rhs); }

float   Vector3::LengthSq()   const { return x * x + y * y + z * z; }
float   Vector3::Length()     const { return std::sqrt(LengthSq()); }
Vector3 Vector3::Normalized() const {
    float len = Length();
    // 契約違反でも実行は止めない。0 を返すのは NaN と違って «伝播しても検出できる» ため。
    FBZZ_MATH_CONTRACT(!NearlyZero(len),
                       "zero-length vector normalized; returning (0,0,0). "
                       "use NormalizedOr(fallback) where degenerate input is normal");
    if (NearlyZero(len)) return ZERO;
    return *this / len;
}

Vector3 Vector3::NormalizedOr(const Vector3& fallback) const {
    float len = Length();
    if (NearlyZero(len)) return fallback;
    return *this / len;
}

float Vector3::Dot(const Vector3& a, const Vector3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector3 Vector3::Cross(const Vector3& a, const Vector3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

Vector3 Vector3::Lerp(const Vector3& a, const Vector3& b, float t) {
    return a + (b - a) * t;
}

} // namespace fbzz::math
