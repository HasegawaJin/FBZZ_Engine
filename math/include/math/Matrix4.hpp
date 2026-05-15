// FBZZ Engine
// Matrix4.hpp | fbzz::math
// 4x4行列 (ワールド・ビュー・プロジェクション行列)
#pragma once

#include "Vector3.hpp"
#include "Vector4.hpp"
#include "Quaternion.hpp"
#include "Matrix3.hpp"

namespace fbzz::math {

struct Matrix4 {
    // m[row][col], 行優先
    float m[4][4] = {};

    constexpr Matrix4() = default;

    static Matrix4 Identity();
    static Matrix4 Zero();

    static Matrix4 Translate(const Vector3& t);
    static Matrix4 Rotate(const Quaternion& q);
    static Matrix4 Scale(const Vector3& s);
    static Matrix4 TRS(const Vector3& t, const Quaternion& r, const Vector3& s);

    // DirectX 左手系
    static Matrix4 LookAt(const Vector3& eye, const Vector3& target, const Vector3& up);
    static Matrix4 Perspective(float fovY, float aspect, float nearZ, float farZ);
    static Matrix4 Orthographic(float left, float right,
                                 float bottom, float top,
                                 float nearZ, float farZ);

    Matrix4 operator*(const Matrix4& rhs) const;
    Vector4 operator*(const Vector4& v)   const;

    static Matrix4 Transpose(const Matrix4& mat);
    static Matrix4 Inverse(const Matrix4& mat);

    // シェーダーへ渡す前に呼ぶ (HLSL は Column-Major)
    Matrix4 Transposed() const;
};

} // namespace fbzz::math
