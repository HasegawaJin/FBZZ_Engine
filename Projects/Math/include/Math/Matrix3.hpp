/// @file    Matrix3.hpp
/// @brief   3x3行列 (法線変換・回転抽出)。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once

#include "Vector3.hpp"

namespace fbzz::math {

struct Matrix4;

struct Matrix3 {
    float m[3][3] = {};

    constexpr Matrix3() = default;

    static Matrix3 Identity();
    static Matrix3 Transpose(const Matrix3& mat);
    // 非一様スケールを含む行列では法線変換に使えない。その場合は Inverse(mat3).Transposed() を使う
    static Matrix3 FromMatrix4(const Matrix4& mat);

    Matrix3 operator*(const Matrix3& rhs) const;
    Vector3 operator*(const Vector3& v)   const;
};

} // namespace fbzz::math
