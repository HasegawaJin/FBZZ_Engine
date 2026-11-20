/// @file    Matrix4.cpp
/// @brief   4x4行列の演算実装 (DirectX 左手系)。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Math/Matrix4.hpp"
#include "Math/MathContract.hpp"
#include "Math/MathUtils.hpp"

#include <cmath>

namespace fbzz::math {

Matrix4 Matrix4::Rotate(const Quaternion& q) {
    const Quaternion unit = q.Normalized();
    const float xx = unit.x * unit.x;
    const float yy = unit.y * unit.y;
    const float zz = unit.z * unit.z;
    const float xy = unit.x * unit.y;
    const float xz = unit.x * unit.z;
    const float yz = unit.y * unit.z;
    const float wx = unit.w * unit.x;
    const float wy = unit.w * unit.y;
    const float wz = unit.w * unit.z;

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

Matrix4 Matrix4::TRS(const Vector3& translation, const Quaternion& rotation, const Vector3& scale) {
    /// @note T * R * S を展開し、回転の各列にスケールを掛けて第 4 列に平行移動を置く。
    Matrix4 result = Rotate(rotation);
    for (int row = 0; row < 3; ++row) {
        result.m[row][0] *= scale.x;
        result.m[row][1] *= scale.y;
        result.m[row][2] *= scale.z;
    }
    result.m[0][3] = translation.x;
    result.m[1][3] = translation.y;
    result.m[2][3] = translation.z;
    return result;
}

Matrix4 Matrix4::LookAt(const Vector3& eye, const Vector3& target, const Vector3& up) {
    const Vector3 forward = (target - eye).Normalized();
    const Vector3 right = Vector3::Cross(up, forward).Normalized();
    const Vector3 correctedUp = Vector3::Cross(forward, right);

    Matrix4 result;
    result.m[0][0] = right.x;
    result.m[0][1] = right.y;
    result.m[0][2] = right.z;
    result.m[0][3] = -Vector3::Dot(right, eye);

    result.m[1][0] = correctedUp.x;
    result.m[1][1] = correctedUp.y;
    result.m[1][2] = correctedUp.z;
    result.m[1][3] = -Vector3::Dot(correctedUp, eye);

    result.m[2][0] = forward.x;
    result.m[2][1] = forward.y;
    result.m[2][2] = forward.z;
    result.m[2][3] = -Vector3::Dot(forward, eye);
    result.m[3][3] = 1.0f;
    return result;
}

Matrix4 Matrix4::Perspective(float fovY, float aspect, float nearZ, float farZ) {
    FBZZ_MATH_CONTRACT(aspect > EPSILON, "degenerate aspect; clamped to 1.0");
    if (!(aspect > EPSILON)) {
        aspect = 1.0f;
    }
    FBZZ_MATH_CONTRACT(farZ > nearZ, "far <= near; far pushed past near");
    if (!(farZ > nearZ)) {
        farZ = nearZ + 1.0f;
    }

    const float yScale = 1.0f / std::tan(fovY * 0.5f);
    const float xScale = yScale / aspect;
    const float depthRange = farZ - nearZ;

    Matrix4 result;
    result.m[0][0] = xScale;
    result.m[1][1] = yScale;
    result.m[2][2] = farZ / depthRange;
    result.m[2][3] = -nearZ * farZ / depthRange;
    result.m[3][2] = 1.0f;
    return result;
}

Matrix4 Matrix4::Orthographic(float left, float right,
                             float bottom, float top,
                             float nearZ, float farZ) {
    const float width = right - left;
    const float height = top - bottom;
    const float depthRange = farZ - nearZ;

    Matrix4 result;
    result.m[0][0] = 2.0f / width;
    result.m[1][1] = 2.0f / height;
    result.m[2][2] = 1.0f / depthRange;
    result.m[0][3] = -(right + left) / width;
    result.m[1][3] = -(top + bottom) / height;
    result.m[2][3] = -nearZ / depthRange;
    result.m[3][3] = 1.0f;
    return result;
}

Matrix4 Matrix4::PerspectiveReversedZ(float fovY, float aspect, float nearZ, float farZ) {
    /// @note Perspective の z 行を (w - z) に置き換えた形。z_ndc = n (f - z) / (z (f - n))。
    /// @see https://developer.nvidia.com/content/depth-precision-visualized (NVIDIA, "Depth Precision Visualized")
    Matrix4 result = Perspective(fovY, aspect, nearZ, farZ);
    if (!(farZ > nearZ)) {
        farZ = nearZ + 1.0f;
    }
    const float depthRange = farZ - nearZ;
    result.m[2][2] = -nearZ / depthRange;
    result.m[2][3] = nearZ * farZ / depthRange;
    return result;
}

Matrix4 Matrix4::OrthographicReversedZ(float left, float right,
                                      float bottom, float top,
                                      float nearZ, float farZ) {
    Matrix4 result = Orthographic(left, right, bottom, top, nearZ, farZ);
    const float depthRange = farZ - nearZ;
    result.m[2][2] = -1.0f / depthRange;
    result.m[2][3] = farZ / depthRange;
    return result;
}

namespace {

/// @brief 2x2 行列 (行優先で 1 レジスタに [a0 a1; a2 a3]) の積 A * B。
inline simd::Vec Mat2Mul(simd::Vec a, simd::Vec b) {
    return _mm_add_ps(_mm_mul_ps(a, _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 0, 3, 0))),
                      _mm_mul_ps(_mm_shuffle_ps(a, a, _MM_SHUFFLE(2, 3, 0, 1)),
                                 _mm_shuffle_ps(b, b, _MM_SHUFFLE(1, 2, 1, 2))));
}

/// @brief 2x2 の adj(A) * B。
inline simd::Vec Mat2AdjMul(simd::Vec a, simd::Vec b) {
    return _mm_sub_ps(_mm_mul_ps(_mm_shuffle_ps(a, a, _MM_SHUFFLE(0, 0, 3, 3)), b),
                      _mm_mul_ps(_mm_shuffle_ps(a, a, _MM_SHUFFLE(2, 2, 1, 1)),
                                 _mm_shuffle_ps(b, b, _MM_SHUFFLE(1, 0, 3, 2))));
}

/// @brief 2x2 の A * adj(B)。
inline simd::Vec Mat2MulAdj(simd::Vec a, simd::Vec b) {
    return _mm_sub_ps(_mm_mul_ps(a, _mm_shuffle_ps(b, b, _MM_SHUFFLE(0, 3, 0, 3))),
                      _mm_mul_ps(_mm_shuffle_ps(a, a, _MM_SHUFFLE(2, 3, 0, 1)),
                                 _mm_shuffle_ps(b, b, _MM_SHUFFLE(1, 2, 1, 2))));
}

}

/// @brief 2x2 ブロック [A B; C D] の余因子で 4x4 逆行列を求める。
/// @note |M| = |A||D| + |B||C| - tr(adj(A)B adj(D)C)。各ブロックの余因子を 1 レジスタ (2x2) ずつ並列に作る。
/// @see https://lxjk.github.io/2017/09/03/Fast-4x4-Matrix-Inverse-with-SSE-SIMD-Explained.html Eric Zhang «Fast 4x4 Matrix Inverse with SSE SIMD, Explained» (General Matrix Inverse)
Matrix4 Matrix4::Inverse(const Matrix4& mat) {
    const simd::Vec row0 = simd::Load4(mat.m[0]);
    const simd::Vec row1 = simd::Load4(mat.m[1]);
    const simd::Vec row2 = simd::Load4(mat.m[2]);
    const simd::Vec row3 = simd::Load4(mat.m[3]);

    const simd::Vec a = _mm_movelh_ps(row0, row1);
    const simd::Vec b = _mm_movehl_ps(row1, row0);
    const simd::Vec c = _mm_movelh_ps(row2, row3);
    const simd::Vec d = _mm_movehl_ps(row3, row2);

    /// @note A, B, C, D の順に各ブロックの対角と反対角の成分を集め、全ブロックの行列式を同時に求める。
    const simd::Vec topLeft = _mm_shuffle_ps(row0, row2, _MM_SHUFFLE(2, 0, 2, 0));
    const simd::Vec topRight = _mm_shuffle_ps(row0, row2, _MM_SHUFFLE(3, 1, 3, 1));
    const simd::Vec bottomLeft = _mm_shuffle_ps(row1, row3, _MM_SHUFFLE(2, 0, 2, 0));
    const simd::Vec bottomRight = _mm_shuffle_ps(row1, row3, _MM_SHUFFLE(3, 1, 3, 1));
    const simd::Vec blockDeterminants = _mm_sub_ps(_mm_mul_ps(topLeft, bottomRight),
                                                   _mm_mul_ps(topRight, bottomLeft));
    const simd::Vec detA = simd::SplatLane<0>(blockDeterminants);
    const simd::Vec detB = simd::SplatLane<1>(blockDeterminants);
    const simd::Vec detC = simd::SplatLane<2>(blockDeterminants);
    const simd::Vec detD = simd::SplatLane<3>(blockDeterminants);

    const simd::Vec adjDC = Mat2AdjMul(d, c);
    const simd::Vec adjAB = Mat2AdjMul(a, b);
    /// @note 逆行列を |M|^-1 [X Y; Z W] と置いたときの各ブロックの余因子。
    const simd::Vec adjX = _mm_sub_ps(_mm_mul_ps(detD, a), Mat2Mul(b, adjDC));
    const simd::Vec adjW = _mm_sub_ps(_mm_mul_ps(detA, d), Mat2Mul(c, adjAB));
    const simd::Vec adjY = _mm_sub_ps(_mm_mul_ps(detB, c), Mat2MulAdj(d, adjAB));
    const simd::Vec adjZ = _mm_sub_ps(_mm_mul_ps(detC, b), Mat2MulAdj(a, adjDC));

    const simd::Vec transposedAdjDC = _mm_shuffle_ps(adjDC, adjDC, _MM_SHUFFLE(3, 1, 2, 0));
    const simd::Vec traceTerms = _mm_mul_ps(adjAB, transposedAdjDC);
    const simd::Vec tracePairs = _mm_hadd_ps(traceTerms, traceTerms);
    const simd::Vec trace = _mm_hadd_ps(tracePairs, tracePairs);
    const simd::Vec determinantProducts = _mm_add_ps(_mm_mul_ps(detA, detD), _mm_mul_ps(detB, detC));
    const simd::Vec determinant = _mm_sub_ps(determinantProducts, trace);

    const float det = _mm_cvtss_f32(determinant);
    FBZZ_MATH_CONTRACT(!NearlyZero(det),
                       "singular matrix inverted (zero scale?); returning identity");
    if (NearlyZero(det)) {
        return Identity();
    }

    /// @note 余因子から元のブロックへ戻す adj の符号 (+ - - +) を 1/|M| に畳む。
    const simd::Vec signedInverseDet = _mm_div_ps(_mm_setr_ps(1.0f, -1.0f, -1.0f, 1.0f), determinant);
    const simd::Vec blockX = _mm_mul_ps(adjX, signedInverseDet);
    const simd::Vec blockY = _mm_mul_ps(adjY, signedInverseDet);
    const simd::Vec blockZ = _mm_mul_ps(adjZ, signedInverseDet);
    const simd::Vec blockW = _mm_mul_ps(adjW, signedInverseDet);

    /// @note adj の並べ替え (成分 0 と 3 の交換) と、ブロックを行へ戻す並べ替えを 1 回のシャッフルで兼ねる。
    Matrix4 result;
    simd::Store4(result.m[0], _mm_shuffle_ps(blockX, blockY, _MM_SHUFFLE(1, 3, 1, 3)));
    simd::Store4(result.m[1], _mm_shuffle_ps(blockX, blockY, _MM_SHUFFLE(0, 2, 0, 2)));
    simd::Store4(result.m[2], _mm_shuffle_ps(blockZ, blockW, _MM_SHUFFLE(1, 3, 1, 3)));
    simd::Store4(result.m[3], _mm_shuffle_ps(blockZ, blockW, _MM_SHUFFLE(0, 2, 0, 2)));
    return result;
}

Matrix4 Matrix4::InverseTransposeAffine(const Matrix4& mat) {
    /// @note 列ベクトル規約の M=[A t; 0 1] では、逆転置の左上 3x3 は cofactor(A) / det(A)。
    const auto& a = mat.m;

    const float c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1];
    const float c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2];
    const float c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
    const float determinant = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
    if (NearlyZero(determinant)) {
        return Identity();
    }

    const float c10 = a[0][2] * a[2][1] - a[0][1] * a[2][2];
    const float c11 = a[0][0] * a[2][2] - a[0][2] * a[2][0];
    const float c12 = a[0][1] * a[2][0] - a[0][0] * a[2][1];
    const float c20 = a[0][1] * a[1][2] - a[0][2] * a[1][1];
    const float c21 = a[0][2] * a[1][0] - a[0][0] * a[1][2];
    const float c22 = a[0][0] * a[1][1] - a[0][1] * a[1][0];

    const float inverseDeterminant = 1.0f / determinant;
    Matrix4 result = Identity();
    result.m[0][0] = c00 * inverseDeterminant;
    result.m[0][1] = c01 * inverseDeterminant;
    result.m[0][2] = c02 * inverseDeterminant;
    result.m[1][0] = c10 * inverseDeterminant;
    result.m[1][1] = c11 * inverseDeterminant;
    result.m[1][2] = c12 * inverseDeterminant;
    result.m[2][0] = c20 * inverseDeterminant;
    result.m[2][1] = c21 * inverseDeterminant;
    result.m[2][2] = c22 * inverseDeterminant;
    return result;
}

}
