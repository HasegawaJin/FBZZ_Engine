/// @file    Matrix4Inverse.cpp
/// @brief   一般行列の逆行列とアフィン変換の法線行列。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "Math/LinearAlgebra/Matrix4.hpp"
#include "Math/Scalar/MathContract.hpp"
#include "Math/Scalar/MathUtils.hpp"
#include <cmath>

namespace fbzz::math {

namespace {

/// @brief 2x2 行列 (行優先で 1 レジスタに [a0 a1; a2 a3]) の積 A * B。
inline simd::Vec MultiplyBlocks(simd::Vec left, simd::Vec right) {
    return _mm_add_ps(_mm_mul_ps(left, _mm_shuffle_ps(right, right, _MM_SHUFFLE(3, 0, 3, 0))),
                      _mm_mul_ps(_mm_shuffle_ps(left, left, _MM_SHUFFLE(2, 3, 0, 1)),
                                 _mm_shuffle_ps(right, right, _MM_SHUFFLE(1, 2, 1, 2))));
}

/// @brief 2x2 の adj(A) * B。
inline simd::Vec MultiplyLeftAdjugate(simd::Vec left, simd::Vec right) {
    return _mm_sub_ps(_mm_mul_ps(_mm_shuffle_ps(left, left, _MM_SHUFFLE(0, 0, 3, 3)), right),
                      _mm_mul_ps(_mm_shuffle_ps(left, left, _MM_SHUFFLE(2, 2, 1, 1)),
                                 _mm_shuffle_ps(right, right, _MM_SHUFFLE(1, 0, 3, 2))));
}

/// @brief 2x2 の A * adj(B)。
inline simd::Vec MultiplyRightAdjugate(simd::Vec left, simd::Vec right) {
    return _mm_sub_ps(_mm_mul_ps(left, _mm_shuffle_ps(right, right, _MM_SHUFFLE(0, 3, 0, 3))),
                      _mm_mul_ps(_mm_shuffle_ps(left, left, _MM_SHUFFLE(2, 3, 0, 1)),
                                 _mm_shuffle_ps(right, right, _MM_SHUFFLE(1, 2, 1, 2))));
}

} /// @note namespace

/// @brief 2x2 ブロック [A B; C D] の余因子で 4x4 逆行列を求める。
/// @note |M| = |A||D| + |B||C| - tr(adj(A)B adj(D)C)。各ブロックの余因子を 1 レジスタ (2x2) ずつ並列に作る。
/// @see https://lxjk.github.io/2017/09/03/Fast-4x4-Matrix-Inverse-with-SSE-SIMD-Explained.html Eric Zhang «Fast 4x4 Matrix Inverse with SSE SIMD, Explained» (General Matrix Inverse)
Matrix4 Matrix4::Inverse(const Matrix4& matrix) {
    const simd::Vec row0 = simd::Load4(matrix.m[0]);
    const simd::Vec row1 = simd::Load4(matrix.m[1]);
    const simd::Vec row2 = simd::Load4(matrix.m[2]);
    const simd::Vec row3 = simd::Load4(matrix.m[3]);

    const simd::Vec upperLeftBlock = _mm_movelh_ps(row0, row1);
    const simd::Vec upperRightBlock = _mm_movehl_ps(row1, row0);
    const simd::Vec lowerLeftBlock = _mm_movelh_ps(row2, row3);
    const simd::Vec lowerRightBlock = _mm_movehl_ps(row3, row2);

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

    const simd::Vec lowerLeftProduct = MultiplyLeftAdjugate(lowerRightBlock, lowerLeftBlock);
    const simd::Vec upperRightProduct = MultiplyLeftAdjugate(upperLeftBlock, upperRightBlock);
    /// @note 逆行列を |M|^-1 [X Y; Z W] と置いたときの各ブロックの余因子。
    const simd::Vec adjX = _mm_sub_ps(_mm_mul_ps(detD, upperLeftBlock), MultiplyBlocks(upperRightBlock, lowerLeftProduct));
    const simd::Vec adjW = _mm_sub_ps(_mm_mul_ps(detA, lowerRightBlock), MultiplyBlocks(lowerLeftBlock, upperRightProduct));
    const simd::Vec adjY = _mm_sub_ps(_mm_mul_ps(detB, lowerLeftBlock), MultiplyRightAdjugate(lowerRightBlock, upperRightProduct));
    const simd::Vec adjZ = _mm_sub_ps(_mm_mul_ps(detC, upperRightBlock), MultiplyRightAdjugate(upperLeftBlock, lowerLeftProduct));

    const simd::Vec transposedLowerLeftProduct = _mm_shuffle_ps(lowerLeftProduct, lowerLeftProduct, _MM_SHUFFLE(3, 1, 2, 0));
    const simd::Vec traceTerms = _mm_mul_ps(upperRightProduct, transposedLowerLeftProduct);
    const simd::Vec tracePairs = _mm_hadd_ps(traceTerms, traceTerms);
    const simd::Vec trace = _mm_hadd_ps(tracePairs, tracePairs);
    const simd::Vec determinantProducts = _mm_add_ps(_mm_mul_ps(detA, detD), _mm_mul_ps(detB, detC));
    const simd::Vec determinant = _mm_sub_ps(determinantProducts, trace);

    const float determinantValue = _mm_cvtss_f32(determinant);
    FBZZ_MATH_CONTRACT(!NearlyZero(determinantValue),
                       "singular matrix inverted (zero scale?); returning identity");
    if (NearlyZero(determinantValue)) {
        return Identity();
    }

    /// @note 余因子から元のブロックへ戻す adj の符号 (+ - - +) を 1/|M| に畳む。
    const simd::Vec signedInverseDeterminant = _mm_div_ps(_mm_setr_ps(1.0f, -1.0f, -1.0f, 1.0f), determinant);
    const simd::Vec blockX = _mm_mul_ps(adjX, signedInverseDeterminant);
    const simd::Vec blockY = _mm_mul_ps(adjY, signedInverseDeterminant);
    const simd::Vec blockZ = _mm_mul_ps(adjZ, signedInverseDeterminant);
    const simd::Vec blockW = _mm_mul_ps(adjW, signedInverseDeterminant);

    /// @note adj の並べ替え (成分 0 と 3 の交換) と、ブロックを行へ戻す並べ替えを 1 回のシャッフルで兼ねる。
    Matrix4 result;
    simd::Store4(result.m[0], _mm_shuffle_ps(blockX, blockY, _MM_SHUFFLE(1, 3, 1, 3)));
    simd::Store4(result.m[1], _mm_shuffle_ps(blockX, blockY, _MM_SHUFFLE(0, 2, 0, 2)));
    simd::Store4(result.m[2], _mm_shuffle_ps(blockZ, blockW, _MM_SHUFFLE(1, 3, 1, 3)));
    simd::Store4(result.m[3], _mm_shuffle_ps(blockZ, blockW, _MM_SHUFFLE(0, 2, 0, 2)));
    return result;
}

Matrix4 Matrix4::InverseTransposeAffine(const Matrix4& matrix) {
    /// @note 列ベクトル規約の M=[A t; 0 1] では、逆転置の左上 3x3 は cofactor(A) / det(A)。
    const auto& linearTransform = matrix.m;

    const float c00 = linearTransform[1][1] * linearTransform[2][2] - linearTransform[1][2] * linearTransform[2][1];
    const float c01 = linearTransform[1][2] * linearTransform[2][0] - linearTransform[1][0] * linearTransform[2][2];
    const float c02 = linearTransform[1][0] * linearTransform[2][1] - linearTransform[1][1] * linearTransform[2][0];
    const float determinant = linearTransform[0][0] * c00 + linearTransform[0][1] * c01 + linearTransform[0][2] * c02;
    if (NearlyZero(determinant)) {
        return Identity();
    }

    const float c10 = linearTransform[0][2] * linearTransform[2][1] - linearTransform[0][1] * linearTransform[2][2];
    const float c11 = linearTransform[0][0] * linearTransform[2][2] - linearTransform[0][2] * linearTransform[2][0];
    const float c12 = linearTransform[0][1] * linearTransform[2][0] - linearTransform[0][0] * linearTransform[2][1];
    const float c20 = linearTransform[0][1] * linearTransform[1][2] - linearTransform[0][2] * linearTransform[1][1];
    const float c21 = linearTransform[0][2] * linearTransform[1][0] - linearTransform[0][0] * linearTransform[1][2];
    const float c22 = linearTransform[0][0] * linearTransform[1][1] - linearTransform[0][1] * linearTransform[1][0];

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

} /// @note namespace fbzz::math
