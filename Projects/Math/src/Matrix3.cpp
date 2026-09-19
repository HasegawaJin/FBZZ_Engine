/// @file    Matrix3.cpp
/// @brief   3x3行列の演算実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Math/Matrix3.hpp"
#include "Math/Matrix4.hpp"

namespace fbzz::math {

Matrix3 Matrix3::FromMatrix4(const Matrix4& mat) {
    Matrix3 result;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            result.m[r][c] = mat.m[r][c];
    return result;
}

} // namespace fbzz::math
