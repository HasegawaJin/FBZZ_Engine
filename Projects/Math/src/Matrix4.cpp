// FBZZ Engine
// Matrix4.cpp | fbzz::math
// 4x4行列の演算実装 (DirectX 左手系)
#include "Math/Matrix4.hpp"
#include "Math/MathUtils.hpp"
#include <cmath>
#include <cassert>

namespace fbzz::math {

Matrix4 Matrix4::Identity() {
    Matrix4 result;
    result.m[0][0] = result.m[1][1] = result.m[2][2] = result.m[3][3] = 1.0f;
    return result;
}

Matrix4 Matrix4::Zero() { return {}; }

Matrix4 Matrix4::Translate(const Vector3& t) {
    Matrix4 result = Identity();
    result.m[0][3] = t.x;
    result.m[1][3] = t.y;
    result.m[2][3] = t.z;
    return result;
}

Matrix4 Matrix4::Rotate(const Quaternion& q) {
    Quaternion qn = q.Normalized();
    float xx = qn.x * qn.x, yy = qn.y * qn.y, zz = qn.z * qn.z;
    float xy = qn.x * qn.y, xz = qn.x * qn.z, yz = qn.y * qn.z;
    float wx = qn.w * qn.x, wy = qn.w * qn.y, wz = qn.w * qn.z;

    Matrix4 result = Identity();
    result.m[0][0] = 1.0f - 2.0f * (yy + zz);
    result.m[0][1] = 2.0f * (xy - wz);
    result.m[0][2] = 2.0f * (xz + wy);
    result.m[1][0] = 2.0f * (xy + wz);
    result.m[1][1] = 1.0f - 2.0f * (xx + zz);
    result.m[1][2] = 2.0f * (yz - wx);
    result.m[2][0] = 2.0f * (xz - wy);
    result.m[2][1] = 2.0f * (yz + wx);
    result.m[2][2] = 1.0f - 2.0f * (xx + yy);
    return result;
}

Matrix4 Matrix4::Scale(const Vector3& s) {
    Matrix4 result = Identity();
    result.m[0][0] = s.x;
    result.m[1][1] = s.y;
    result.m[2][2] = s.z;
    return result;
}

Matrix4 Matrix4::TRS(const Vector3& t, const Quaternion& r, const Vector3& s) {
    return Translate(t) * Rotate(r) * Scale(s);
}

Matrix4 Matrix4::LookAt(const Vector3& eye, const Vector3& target, const Vector3& up) {
    // DirectX 左手系 LookAt
    Vector3 z = (target - eye).Normalized();   // forward (+Z into screen)
    Vector3 x = Vector3::Cross(up, z).Normalized(); // right
    Vector3 y = Vector3::Cross(z, x);              // corrected up

    Matrix4 result;
    result.m[0][0] = x.x; result.m[0][1] = x.y; result.m[0][2] = x.z;
    result.m[0][3] = -Vector3::Dot(x, eye);
    result.m[1][0] = y.x; result.m[1][1] = y.y; result.m[1][2] = y.z;
    result.m[1][3] = -Vector3::Dot(y, eye);
    result.m[2][0] = z.x; result.m[2][1] = z.y; result.m[2][2] = z.z;
    result.m[2][3] = -Vector3::Dot(z, eye);
    result.m[3][3] = 1.0f;
    return result;
}

Matrix4 Matrix4::Perspective(float fovY, float aspect, float nearZ, float farZ) {
    // DirectX 左手系 透視投影 (depth: 0 to 1)
    assert(aspect > EPSILON);
    assert(farZ > nearZ);
    float yScale = 1.0f / std::tan(fovY * 0.5f);
    float xScale = yScale / aspect;

    Matrix4 result;
    result.m[0][0] = xScale;
    result.m[1][1] = yScale;
    result.m[2][2] = farZ / (farZ - nearZ);
    result.m[2][3] = -nearZ * farZ / (farZ - nearZ);
    result.m[3][2] = 1.0f;
    return result;
}

Matrix4 Matrix4::Orthographic(float left, float right,
                               float bottom, float top,
                               float nearZ, float farZ) {
    Matrix4 result;
    result.m[0][0] = 2.0f / (right - left);
    result.m[1][1] = 2.0f / (top - bottom);
    result.m[2][2] = 1.0f / (farZ - nearZ);
    result.m[0][3] = -(right + left) / (right - left);
    result.m[1][3] = -(top + bottom) / (top - bottom);
    result.m[2][3] = -nearZ / (farZ - nearZ);
    result.m[3][3] = 1.0f;
    return result;
}

Matrix4 Matrix4::operator*(const Matrix4& rhs) const {
    Matrix4 result;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k)
                result.m[r][c] += m[r][k] * rhs.m[k][c];
    return result;
}

Vector4 Matrix4::operator*(const Vector4& v) const {
    return {
        m[0][0]*v.x + m[0][1]*v.y + m[0][2]*v.z + m[0][3]*v.w,
        m[1][0]*v.x + m[1][1]*v.y + m[1][2]*v.z + m[1][3]*v.w,
        m[2][0]*v.x + m[2][1]*v.y + m[2][2]*v.z + m[2][3]*v.w,
        m[3][0]*v.x + m[3][1]*v.y + m[3][2]*v.z + m[3][3]*v.w
    };
}

Matrix4 Matrix4::Transpose(const Matrix4& mat) {
    Matrix4 result;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            result.m[r][c] = mat.m[c][r];
    return result;
}

// 余因子展開による 4x4 逆行列
Matrix4 Matrix4::Inverse(const Matrix4& mat) {
    const float* a = &mat.m[0][0];

    float b00 = a[ 0]*a[ 5] - a[ 1]*a[ 4];
    float b01 = a[ 0]*a[ 6] - a[ 2]*a[ 4];
    float b02 = a[ 0]*a[ 7] - a[ 3]*a[ 4];
    float b03 = a[ 1]*a[ 6] - a[ 2]*a[ 5];
    float b04 = a[ 1]*a[ 7] - a[ 3]*a[ 5];
    float b05 = a[ 2]*a[ 7] - a[ 3]*a[ 6];
    float b06 = a[ 8]*a[13] - a[ 9]*a[12];
    float b07 = a[ 8]*a[14] - a[10]*a[12];
    float b08 = a[ 8]*a[15] - a[11]*a[12];
    float b09 = a[ 9]*a[14] - a[10]*a[13];
    float b10 = a[ 9]*a[15] - a[11]*a[13];
    float b11 = a[10]*a[15] - a[11]*a[14];

    float det = b00*b11 - b01*b10 + b02*b09 + b03*b08 - b04*b07 + b05*b06;
    assert(!NearlyZero(det) && "Matrix4::Inverse: singular matrix");

    float inv = 1.0f / det;
    Matrix4 result;
    float* r = &result.m[0][0];

    r[ 0] = ( a[ 5]*b11 - a[ 6]*b10 + a[ 7]*b09) * inv;
    r[ 1] = (-a[ 1]*b11 + a[ 2]*b10 - a[ 3]*b09) * inv;
    r[ 2] = ( a[13]*b05 - a[14]*b04 + a[15]*b03) * inv;
    r[ 3] = (-a[ 9]*b05 + a[10]*b04 - a[11]*b03) * inv;
    r[ 4] = (-a[ 4]*b11 + a[ 6]*b08 - a[ 7]*b07) * inv;
    r[ 5] = ( a[ 0]*b11 - a[ 2]*b08 + a[ 3]*b07) * inv;
    r[ 6] = (-a[12]*b05 + a[14]*b02 - a[15]*b01) * inv;
    r[ 7] = ( a[ 8]*b05 - a[10]*b02 + a[11]*b01) * inv;
    r[ 8] = ( a[ 4]*b10 - a[ 5]*b08 + a[ 7]*b06) * inv;
    r[ 9] = (-a[ 0]*b10 + a[ 1]*b08 - a[ 3]*b06) * inv;
    r[10] = ( a[12]*b04 - a[13]*b02 + a[15]*b00) * inv;
    r[11] = (-a[ 8]*b04 + a[ 9]*b02 - a[11]*b00) * inv;
    r[12] = (-a[ 4]*b09 + a[ 5]*b07 - a[ 6]*b06) * inv;
    r[13] = ( a[ 0]*b09 - a[ 1]*b07 + a[ 2]*b06) * inv;
    r[14] = (-a[12]*b03 + a[13]*b01 - a[14]*b00) * inv;
    r[15] = ( a[ 8]*b03 - a[ 9]*b01 + a[10]*b00) * inv;

    return result;
}

Matrix4 Matrix4::InverseTransposeAffine(const Matrix4& mat) {
    // アフィン行列 M = [[A, 0], [t, 1]] (行優先・行ベクトル規約) の逆行列は
    //   M^-1 = [[A^-1, 0], [-t*A^-1, 1]]
    // なので、その転置の左上 3x3 は (A^-1)^T になる。平行移動 t は一切効かない。
    // さらに A^-1 = adj(A)/det = cofactor(A)^T/det より (A^-1)^T = cofactor(A)/det。
    // つまり左上 3x3 の余因子行列を行列式で割るだけでよい。
    const auto& a = mat.m;

    const float c00 = a[1][1]*a[2][2] - a[1][2]*a[2][1];
    const float c01 = a[1][2]*a[2][0] - a[1][0]*a[2][2];
    const float c02 = a[1][0]*a[2][1] - a[1][1]*a[2][0];

    const float det = a[0][0]*c00 + a[0][1]*c01 + a[0][2]*c02;
    Matrix4 result = Identity();
    // スケール 0 などで退化した場合は単位行列を返す。
    // WHY assert しないか: Transform のスケールに 0 を入れるのはエディタ操作として普通に起きる。
    //      描画のたびに停止させる類の異常ではないので、法線を素通しして描き続ける。
    if (NearlyZero(det))
        return result;

    const float c10 = a[0][2]*a[2][1] - a[0][1]*a[2][2];
    const float c11 = a[0][0]*a[2][2] - a[0][2]*a[2][0];
    const float c12 = a[0][1]*a[2][0] - a[0][0]*a[2][1];
    const float c20 = a[0][1]*a[1][2] - a[0][2]*a[1][1];
    const float c21 = a[0][2]*a[1][0] - a[0][0]*a[1][2];
    const float c22 = a[0][0]*a[1][1] - a[0][1]*a[1][0];

    const float inv = 1.0f / det;
    result.m[0][0] = c00 * inv; result.m[0][1] = c01 * inv; result.m[0][2] = c02 * inv;
    result.m[1][0] = c10 * inv; result.m[1][1] = c11 * inv; result.m[1][2] = c12 * inv;
    result.m[2][0] = c20 * inv; result.m[2][1] = c21 * inv; result.m[2][2] = c22 * inv;
    return result;
}

Matrix4 Matrix4::Transposed() const { return Transpose(*this); }

} // namespace fbzz::math
