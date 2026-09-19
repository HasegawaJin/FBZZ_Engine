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
    /// @brief Matrix4 の左上 3x3 を取り出す。
    /// @note 非一様スケールを含む行列は法線変換に使えない。その場合は Inverse(mat3).Transposed() を使う。
    static Matrix3 FromMatrix4(const Matrix4& mat);

    Matrix3 operator*(const Matrix3& rhs) const;
    Vector3 operator*(const Vector3& v)   const;
};

/// @note FBZZMath は DLL のため、.cpp に置くと呼び出しごとに DLL 境界を越えてインライン化されない。小関数はヘッダーで定義する。
inline Matrix3 Matrix3::Identity() {
    Matrix3 result;
    result.m[0][0] = 1.0f;
    result.m[1][1] = 1.0f;
    result.m[2][2] = 1.0f;
    return result;
}

inline Matrix3 Matrix3::Transpose(const Matrix3& mat) {
    Matrix3 result;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            result.m[r][c] = mat.m[c][r];
    return result;
}

inline Matrix3 Matrix3::operator*(const Matrix3& rhs) const {
    Matrix3 result;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
                result.m[r][c] += m[r][k] * rhs.m[k][c];
    return result;
}

inline Vector3 Matrix3::operator*(const Vector3& v) const {
    return {
        m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
        m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
        m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z
    };
}

} // namespace fbzz::math
