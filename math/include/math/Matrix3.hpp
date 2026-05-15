// FBZZ Engine
// Matrix3.hpp | fbzz::math
// 3x3行列 (法線変換・回転抽出)
#pragma once

#include "Vector3.hpp"

namespace fbzz::math {

struct Matrix4;

struct Matrix3 {
    float m[3][3] = {};

    Matrix3() = default;

    static Matrix3 Identity();
    static Matrix3 Transpose(const Matrix3& mat);
    static Matrix3 FromMatrix4(const Matrix4& mat);

    Matrix3 operator*(const Matrix3& rhs) const;
    Vector3 operator*(const Vector3& v)   const;
};

} // namespace fbzz::math
