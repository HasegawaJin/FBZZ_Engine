/// @file    Matrix4.hpp
/// @brief   4x4行列 (ワールド・ビュー・プロジェクション行列)。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once

#include "Vector3.hpp"
#include "Vector4.hpp"
#include "Quaternion.hpp"
#include "Matrix3.hpp"

namespace fbzz::math {

struct Matrix4 {
    /// @brief 要素配列。m[row][col]、行優先。
    float m[4][4] = {};

    constexpr Matrix4() = default;

    static Matrix4 Identity();
    static Matrix4 Zero();

    static Matrix4 Translate(const Vector3& t);
    static Matrix4 Rotate(const Quaternion& q);
    static Matrix4 Scale(const Vector3& s);
    static Matrix4 TRS(const Vector3& t, const Quaternion& r, const Vector3& s);

    /// @note DirectX 左手系。
    static Matrix4 LookAt(const Vector3& eye, const Vector3& target, const Vector3& up);
    /// @note DirectX 左手系。
    static Matrix4 Perspective(float fovY, float aspect, float nearZ, float farZ);
    /// @note DirectX 左手系。
    static Matrix4 Orthographic(float left, float right,
                                 float bottom, float top,
                                 float nearZ, float farZ);

    Matrix4 operator*(const Matrix4& rhs) const;
    Vector4 operator*(const Vector4& v)   const;

    static Matrix4 Transpose(const Matrix4& mat);
    static Matrix4 Inverse(const Matrix4& mat);

    /// @brief 法線変換行列 (逆転置) をアフィン行列限定で安く求める。
    /// @return Transpose(Inverse(mat)) の左上 3x3 と同じ値。平行移動成分は寄与しないため残りは単位行列。
    /// @note 完全な 4x4 逆行列より安い。アフィン行列は左上 3x3 の余因子行列 1 回で足りる。
    /// @note シェーダーは float3x3 へキャストして使うため、左上 3x3 が一致していれば結果は変わらない。
    /// @note 退化行列 (行列式 0) では単位行列を返す。呼び出し側の分岐を不要にするため。
    static Matrix4 InverseTransposeAffine(const Matrix4& mat);

    /// @brief 転置行列を返す。
    /// @note HLSL cbuffer は column-major で読むため、row-major な C++ 側の行列はこれで転置してからアップロードする。
    Matrix4 Transposed() const;
};

/// @note FBZZMath は DLL のため、.cpp に置くと呼び出しごとに DLL 境界を越えてインライン化されない。契約を報告しない小関数はヘッダーで定義する。
inline Matrix4 Matrix4::Identity() {
    Matrix4 result;
    result.m[0][0] = result.m[1][1] = result.m[2][2] = result.m[3][3] = 1.0f;
    return result;
}

inline Matrix4 Matrix4::Zero() { return {}; }

inline Matrix4 Matrix4::Translate(const Vector3& t) {
    Matrix4 result = Identity();
    result.m[0][3] = t.x;
    result.m[1][3] = t.y;
    result.m[2][3] = t.z;
    return result;
}

inline Matrix4 Matrix4::Scale(const Vector3& s) {
    Matrix4 result = Identity();
    result.m[0][0] = s.x;
    result.m[1][1] = s.y;
    result.m[2][2] = s.z;
    return result;
}

inline Matrix4 Matrix4::operator*(const Matrix4& rhs) const {
    Matrix4 result;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k)
                result.m[r][c] += m[r][k] * rhs.m[k][c];
    return result;
}

inline Vector4 Matrix4::operator*(const Vector4& v) const {
    return {
        m[0][0]*v.x + m[0][1]*v.y + m[0][2]*v.z + m[0][3]*v.w,
        m[1][0]*v.x + m[1][1]*v.y + m[1][2]*v.z + m[1][3]*v.w,
        m[2][0]*v.x + m[2][1]*v.y + m[2][2]*v.z + m[2][3]*v.w,
        m[3][0]*v.x + m[3][1]*v.y + m[3][2]*v.z + m[3][3]*v.w
    };
}

inline Matrix4 Matrix4::Transpose(const Matrix4& mat) {
    Matrix4 result;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            result.m[r][c] = mat.m[c][r];
    return result;
}

inline Matrix4 Matrix4::Transposed() const { return Transpose(*this); }

} // namespace fbzz::math
