/// @file    Vector4.cpp
/// @brief   4次元ベクトルの演算実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Math/Vector4.hpp"
#include "Math/MathUtils.hpp"
#include <cmath>
#include "Math/MathContract.hpp"

namespace fbzz::math {

Vector4 Vector4::Normalized() const {
    float len = Length();
    FBZZ_MATH_CONTRACT(!NearlyZero(len),
                       "zero-length vector normalized; returning (0,0,0,0)");
    if (NearlyZero(len)) return ZERO;
    return *this * (1.0f / len);
}

} // namespace fbzz::math
