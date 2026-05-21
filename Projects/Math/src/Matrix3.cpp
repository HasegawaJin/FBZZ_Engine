// FBZZ Engine
// Matrix3.cpp | fbzz::math
// 3x3行列の演算実装
#include "Math/Matrix3.hpp"
#include "Math/Matrix4.hpp"

namespace fbzz::math {

Matrix3 Matrix3::Identity() {
    Matrix3 result;
    result.m[0][0] = 1.0f;
    result.m[1][1] = 1.0f;
    result.m[2][2] = 1.0f;
    return result;
}

Matrix3 Matrix3::Transpose(const Matrix3& mat) {
    Matrix3 result;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            result.m[r][c] = mat.m[c][r];
    return result;
}

Matrix3 Matrix3::FromMatrix4(const Matrix4& mat) {
    Matrix3 result;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            result.m[r][c] = mat.m[r][c];
    return result;
}

Matrix3 Matrix3::operator*(const Matrix3& rhs) const {
    Matrix3 result;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
                result.m[r][c] += m[r][k] * rhs.m[k][c];
    return result;
}

Vector3 Matrix3::operator*(const Vector3& v) const {
    return {
        m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
        m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
        m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z
    };
}

} // namespace fbzz::math
