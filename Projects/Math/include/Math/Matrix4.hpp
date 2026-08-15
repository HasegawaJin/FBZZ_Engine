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

    // 法線変換行列 (逆転置) をアフィン行列限定で安く求める。
    // WHAT: Transpose(Inverse(mat)) の左上 3x3 と同じ値を返す。平行移動成分は法線変換に
    //       寄与しないため、残りの要素は単位行列で埋める。
    // WHY: 描画ループは毎オブジェクトこれを計算する。4x4 の完全な逆行列は 16 個の 3x3 小行列式を
    //      経由するのに対し、アフィン行列なら左上 3x3 の余因子行列 1 回で足りる。
    //      シェーダー側は例外なく (float3x3) へキャストして normalize するので、
    //      左上 3x3 さえ一致していれば結果は変わらない。
    // NOTE: 退化した行列 (行列式 0) では単位行列を返す。呼び出し側での分岐を不要にするため。
    static Matrix4 InverseTransposeAffine(const Matrix4& mat);

    // HLSL cbuffer は column-major で読むため C++ row-major と自動転置される。
    // 列ベクトル形式の行列はそのままアップロードすれば HLSL 側で正しく解釈される。
    Matrix4 Transposed() const;
};

} // namespace fbzz::math
