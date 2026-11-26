/// @file    Vector2.cpp
/// @brief   2次元ベクトルの演算実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Math/LinearAlgebra/Vector2.hpp"
#include "Math/Scalar/MathUtils.hpp"
#include <cmath>
#include "Math/Scalar/MathContract.hpp"

namespace fbzz::math {

Vector2 Vector2::Normalized() const {
    float len = Length();
    FBZZ_MATH_CONTRACT(!NearlyZero(len),
                       "zero-length vector normalized; returning (0,0)");
    if (NearlyZero(len)) {
        return ZERO;
    }
    return *this / len;
}

} // namespace fbzz::math
