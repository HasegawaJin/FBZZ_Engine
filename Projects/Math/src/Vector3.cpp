/// @file    Vector3.cpp
/// @brief   3次元ベクトルの演算実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Math/Vector3.hpp"
#include "Math/MathUtils.hpp"
#include <cmath>
#include "Math/MathContract.hpp"

namespace fbzz::math {

Vector3 Vector3::Normalized() const {
    float len = Length();
    /// @note 契約違反でも実行は止めない。0 は NaN と違い、伝播しても検出できる値のため。
    FBZZ_MATH_CONTRACT(!NearlyZero(len),
                       "zero-length vector normalized; returning (0,0,0). "
                       "use NormalizedOr(fallback) where degenerate input is normal");
    if (NearlyZero(len)) return ZERO;
    return *this / len;
}

} // namespace fbzz::math
