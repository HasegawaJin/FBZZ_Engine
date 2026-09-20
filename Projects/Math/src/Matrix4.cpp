/// @file    Matrix4.cpp
/// @brief   4x4行列の演算実装 (DirectX 左手系)。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Math/Matrix4.hpp"
#include "Math/MathUtils.hpp"
#include <cmath>
#include "Math/MathContract.hpp"

namespace fbzz::math {

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

Matrix4 Matrix4::TRS(const Vector3& t, const Quaternion& r, const Vector3& s) {
    /// @note T*R*S を展開すると «回転行列の列 c に s[c] を掛け、第 4 列に t を置く» だけになる。行列積 2 回 (乗算 128 回) を省く。
    /// @note 積で組んだ場合と値は一致する (展開で消える項は 0 との積と 1 との積だけで、丸めを生まない)。
    Matrix4 result = Rotate(r);
    for (int row = 0; row < 3; ++row) {
        result.m[row][0] *= s.x;
        result.m[row][1] *= s.y;
        result.m[row][2] *= s.z;
    }
    result.m[0][3] = t.x;
    result.m[1][3] = t.y;
    result.m[2][3] = t.z;
    return result;
}

Matrix4 Matrix4::LookAt(const Vector3& eye, const Vector3& target, const Vector3& up) {
    /// @note DirectX 左手系の LookAt。
    /// @note z=forward (+Z 画面奥)、x=right、y=corrected up。
    Vector3 z = (target - eye).Normalized();
    Vector3 x = Vector3::Cross(up, z).Normalized();
    Vector3 y = Vector3::Cross(z, x);

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
    /// @note DirectX 左手系の透視投影 (深度 0..1)。
    /// @note 潰れたビューポート (幅/高さ 0) は編集中に普通に起きる。破綻しない最小値へ寄せて進む。
    FBZZ_MATH_CONTRACT(aspect > EPSILON, "degenerate aspect; clamped to 1.0");
    if (!(aspect > EPSILON)) aspect = 1.0f;
    FBZZ_MATH_CONTRACT(farZ > nearZ, "far <= near; far pushed past near");
    if (!(farZ > nearZ)) farZ = nearZ + 1.0f;

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

Matrix4 Matrix4::PerspectiveReversedZ(float fovY, float aspect, float nearZ, float farZ) {
    /// @note Perspective の z 行を (w - z) に置き換えた形。z_ndc = n (f - z) / (z (f - n))。
    /// @see https://developer.nvidia.com/content/depth-precision-visualized (NVIDIA, "Depth Precision Visualized")
    Matrix4 result = Perspective(fovY, aspect, nearZ, farZ);
    if (!(farZ > nearZ)) farZ = nearZ + 1.0f;
    result.m[2][2] = -nearZ / (farZ - nearZ);
    result.m[2][3] = nearZ * farZ / (farZ - nearZ);
    return result;
}

Matrix4 Matrix4::OrthographicReversedZ(float left, float right,
                                       float bottom, float top,
                                       float nearZ, float farZ) {
    Matrix4 result = Orthographic(left, right, bottom, top, nearZ, farZ);
    result.m[2][2] = -1.0f / (farZ - nearZ);
    result.m[2][3] = farZ / (farZ - nearZ);
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

} // namespace

/// @brief 2x2 ブロック [A B; C D] の余因子で 4x4 逆行列を求める。
/// @note |M| = |A||D| + |B||C| - tr(adj(A)B adj(D)C)。各ブロックの余因子を 1 レジスタ (2x2) ずつ並列に作る。
/// @see https://lxjk.github.io/2017/09/03/Fast-4x4-Matrix-Inverse-with-SSE-SIMD-Explained.html Eric Zhang «Fast 4x4 Matrix Inverse with SSE SIMD, Explained» (General Matrix Inverse)
Matrix4 Matrix4::Inverse(const Matrix4& mat) {
    const simd::Vec r0 = simd::Load4(mat.m[0]);
    const simd::Vec r1 = simd::Load4(mat.m[1]);
    const simd::Vec r2 = simd::Load4(mat.m[2]);
    const simd::Vec r3 = simd::Load4(mat.m[3]);

    const simd::Vec a = _mm_movelh_ps(r0, r1);
    const simd::Vec b = _mm_movehl_ps(r1, r0);
    const simd::Vec c = _mm_movelh_ps(r2, r3);
    const simd::Vec d = _mm_movehl_ps(r3, r2);

    /// @note 4 ブロックの行列式を 1 レジスタで (|A| |B| |C| |D|)。
    const simd::Vec detSub = _mm_sub_ps(
        _mm_mul_ps(_mm_shuffle_ps(r0, r2, _MM_SHUFFLE(2, 0, 2, 0)), _mm_shuffle_ps(r1, r3, _MM_SHUFFLE(3, 1, 3, 1))),
        _mm_mul_ps(_mm_shuffle_ps(r0, r2, _MM_SHUFFLE(3, 1, 3, 1)), _mm_shuffle_ps(r1, r3, _MM_SHUFFLE(2, 0, 2, 0))));
    const simd::Vec detA = simd::SplatLane<0>(detSub);
    const simd::Vec detB = simd::SplatLane<1>(detSub);
    const simd::Vec detC = simd::SplatLane<2>(detSub);
    const simd::Vec detD = simd::SplatLane<3>(detSub);

    const simd::Vec adjDC = Mat2AdjMul(d, c);
    const simd::Vec adjAB = Mat2AdjMul(a, b);
    /// @note 逆行列を |M|^-1 [X Y; Z W] と置いたときの各ブロックの余因子。
    simd::Vec adjX = _mm_sub_ps(_mm_mul_ps(detD, a), Mat2Mul(b, adjDC));
    simd::Vec adjW = _mm_sub_ps(_mm_mul_ps(detA, d), Mat2Mul(c, adjAB));
    simd::Vec adjY = _mm_sub_ps(_mm_mul_ps(detB, c), Mat2MulAdj(d, adjAB));
    simd::Vec adjZ = _mm_sub_ps(_mm_mul_ps(detC, b), Mat2MulAdj(a, adjDC));

    simd::Vec trace = _mm_mul_ps(adjAB, _mm_shuffle_ps(adjDC, adjDC, _MM_SHUFFLE(3, 1, 2, 0)));
    trace = _mm_hadd_ps(trace, trace);
    trace = _mm_hadd_ps(trace, trace);
    const simd::Vec detM = _mm_sub_ps(_mm_add_ps(_mm_mul_ps(detA, detD), _mm_mul_ps(detB, detC)), trace);

    const float det = _mm_cvtss_f32(detM);
    /// @note scale に 0 が入った Transform は特異行列になる。Inspector の操作として普通に起きるため、
    ///       単位行列を返し「その変換が効かない」だけに留める (Decompose 側と同じ判断)。
    FBZZ_MATH_CONTRACT(!NearlyZero(det),
                       "singular matrix inverted (zero scale?); returning identity");
    if (NearlyZero(det)) return Identity();

    /// @note 余因子から元のブロックへ戻す adj の符号 (+ - - +) を 1/|M| に畳む。
    const simd::Vec invDet = _mm_div_ps(_mm_setr_ps(1.0f, -1.0f, -1.0f, 1.0f), detM);
    adjX = _mm_mul_ps(adjX, invDet);
    adjY = _mm_mul_ps(adjY, invDet);
    adjZ = _mm_mul_ps(adjZ, invDet);
    adjW = _mm_mul_ps(adjW, invDet);

    /// @note adj の並べ替え (成分 0 と 3 の交換) と、ブロックを行へ戻す並べ替えを 1 回のシャッフルで兼ねる。
    Matrix4 result;
    simd::Store4(result.m[0], _mm_shuffle_ps(adjX, adjY, _MM_SHUFFLE(1, 3, 1, 3)));
    simd::Store4(result.m[1], _mm_shuffle_ps(adjX, adjY, _MM_SHUFFLE(0, 2, 0, 2)));
    simd::Store4(result.m[2], _mm_shuffle_ps(adjZ, adjW, _MM_SHUFFLE(1, 3, 1, 3)));
    simd::Store4(result.m[3], _mm_shuffle_ps(adjZ, adjW, _MM_SHUFFLE(0, 2, 0, 2)));
    return result;
}

Matrix4 Matrix4::InverseTransposeAffine(const Matrix4& mat) {
    /// @note アフィン行列 M=[[A,0],[t,1]] の逆行列は M^-1=[[A^-1,0],[-t*A^-1,1]]。
    /// @note その転置の左上 3x3 は (A^-1)^T = cofactor(A)/det になる (t は寄与しない)。
    /// @note よって左上 3x3 の余因子行列を行列式で割るだけでよい。
    const auto& a = mat.m;

    const float c00 = a[1][1]*a[2][2] - a[1][2]*a[2][1];
    const float c01 = a[1][2]*a[2][0] - a[1][0]*a[2][2];
    const float c02 = a[1][0]*a[2][1] - a[1][1]*a[2][0];

    const float det = a[0][0]*c00 + a[0][1]*c01 + a[0][2]*c02;
    Matrix4 result = Identity();
    /// @note スケール 0 などの退化は Transform 編集で普通に起きるため assert せず、単位行列を返して進む。
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

} // namespace fbzz::math
