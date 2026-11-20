/// @file    Matrix3.cpp
/// @brief   3x3行列の演算実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Math/Matrix3.hpp"
#include "Math/Matrix4.hpp"

namespace fbzz::math {

Matrix3 Matrix3::FromMatrix4(const Matrix4& mat) {
    Matrix3 result;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result.m[row][column] = mat.m[row][column];
        }
    }
    return result;
}

}
