/// @file    Matrix4.hpp
/// @brief   4x4行列 (ワールド・ビュー・プロジェクション行列)。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once

#include "Vector3.hpp"
#include "Vector4.hpp"
#include "Quaternion.hpp"
#include "Matrix3.hpp"
#include "Simd.hpp"

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

/// @note 結果の行 r = Σk m[r][k] * rhs の行 k。k = 0 から順に足すので、スカラーの三重ループと加算の順序が同じで値も一致する。
inline Matrix4 Matrix4::operator*(const Matrix4& rhs) const {
    const simd::Vec b0 = simd::Load4(rhs.m[0]);
    const simd::Vec b1 = simd::Load4(rhs.m[1]);
    const simd::Vec b2 = simd::Load4(rhs.m[2]);
    const simd::Vec b3 = simd::Load4(rhs.m[3]);
    Matrix4 result;
    for (int r = 0; r < 4; ++r) {
        const simd::Vec a = simd::Load4(m[r]);
        simd::Vec row = _mm_mul_ps(simd::SplatLane<0>(a), b0);
        row = simd::MulAdd(simd::SplatLane<1>(a), b1, row);
        row = simd::MulAdd(simd::SplatLane<2>(a), b2, row);
        row = simd::MulAdd(simd::SplatLane<3>(a), b3, row);
        simd::Store4(result.m[r], row);
    }
    return result;
}

/// @note 行優先に列ベクトルを掛けるので、転置して列を取り出し «列 j × v[j]» を j 順に足す。加算の順序はスカラーの内積と同じで値も一致する。
inline Vector4 Matrix4::operator*(const Vector4& v) const {
    simd::Vec c0 = simd::Load4(m[0]);
    simd::Vec c1 = simd::Load4(m[1]);
    simd::Vec c2 = simd::Load4(m[2]);
    simd::Vec c3 = simd::Load4(m[3]);
    simd::Transpose4x4(c0, c1, c2, c3);
    simd::Vec sum = _mm_mul_ps(c0, _mm_set1_ps(v.x));
    sum = simd::MulAdd(c1, _mm_set1_ps(v.y), sum);
    sum = simd::MulAdd(c2, _mm_set1_ps(v.z), sum);
    sum = simd::MulAdd(c3, _mm_set1_ps(v.w), sum);
    Vector4 result;
    simd::Store4(&result.x, sum);
    return result;
}

inline Matrix4 Matrix4::Transpose(const Matrix4& mat) {
    simd::Vec r0 = simd::Load4(mat.m[0]);
    simd::Vec r1 = simd::Load4(mat.m[1]);
    simd::Vec r2 = simd::Load4(mat.m[2]);
    simd::Vec r3 = simd::Load4(mat.m[3]);
    simd::Transpose4x4(r0, r1, r2, r3);
    Matrix4 result;
    simd::Store4(result.m[0], r0);
    simd::Store4(result.m[1], r1);
    simd::Store4(result.m[2], r2);
    simd::Store4(result.m[3], r3);
    return result;
}

inline Matrix4 Matrix4::Transposed() const { return Transpose(*this); }

} // namespace fbzz::math
