/// @file    Frustum.cpp
/// @brief   視錐台の平面抽出・交差判定実装。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include "Math/Frustum.hpp"
#include "Math/MathContract.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace fbzz::math {

namespace {

/// @brief 4 球ぶんの «外側» ビット (movemask) → visible へ書く 4 バイト (外側 0・交差 1、リトルエンディアン)。
/// @note 1 バイトずつ分岐して書くより、表引き 1 回と 4 バイトの書き込み 1 回のほうが安い。
constexpr std::array<uint32_t, 16> kVisibleBytes = [] {
    std::array<uint32_t, 16> table{};
    for (uint32_t mask = 0; mask < 16; ++mask)
        for (uint32_t k = 0; k < 4; ++k)
            if (((mask >> k) & 1u) == 0) table[mask] |= 1u << (k * 8);
    return table;
}();

} // namespace

/// @note 4 球を転置して «球がレーン・平面がループ» にする。単体版は «平面がレーン» なので、判定の式と加算の順序は同じで結果も一致する。
void Frustum::IntersectsSpheres(std::span<const Vector4> spheres, std::span<uint8_t> visible) const
{
    FBZZ_MATH_CONTRACT(visible.size() >= spheres.size(),
                       "visible is shorter than spheres; judging only what fits");
    const size_t count = std::min(spheres.size(), visible.size());

    /// @note 平面の係数を 4 レーンへ複製して置く。24 本をレジスタ (16 本) に抱えたままにはできないので、ループ内は L1 から読む。
    simd::Vec normalX[PLANE_COUNT];
    simd::Vec normalY[PLANE_COUNT];
    simd::Vec normalZ[PLANE_COUNT];
    simd::Vec distance[PLANE_COUNT];
    for (int p = 0; p < PLANE_COUNT; ++p) {
        normalX[p]  = _mm_set1_ps(m_normalX[p]);
        normalY[p]  = _mm_set1_ps(m_normalY[p]);
        normalZ[p]  = _mm_set1_ps(m_normalZ[p]);
        distance[p] = _mm_set1_ps(m_distance[p]);
    }

    const simd::Vec signBit = _mm_set1_ps(-0.0f);
    size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        simd::Vec x = simd::Load4(&spheres[i].x);
        simd::Vec y = simd::Load4(&spheres[i + 1].x);
        simd::Vec z = simd::Load4(&spheres[i + 2].x);
        simd::Vec r = simd::Load4(&spheres[i + 3].x);
        simd::Transpose4x4(x, y, z, r);
        const simd::Vec negRadius = _mm_xor_ps(r, signBit);

        simd::Vec outside = _mm_setzero_ps();
        for (int p = 0; p < PLANE_COUNT; ++p) {
            simd::Vec d = _mm_mul_ps(normalX[p], x);
            d = simd::MulAdd(normalY[p], y, d);
            d = simd::MulAdd(normalZ[p], z, d);
            d = _mm_add_ps(d, distance[p]);
            outside = _mm_or_ps(outside, _mm_cmplt_ps(d, negRadius));
        }
        const uint32_t bytes = kVisibleBytes[_mm_movemask_ps(outside)];
        std::memcpy(&visible[i], &bytes, sizeof(bytes));
    }
    for (; i < count; ++i)
        visible[i] = IntersectsSphere(spheres[i].XYZ(), spheres[i].w) ? 1 : 0;
}

/// @note Gribb-Hartmann 法。M * v (列ベクトル規則) では clip.i = dot(row_i, world)。
/// @note 各平面の係数は row_3 ± row_i で求まる。DirectX 深度 [0,1] のため near は row_2 のみ。
Frustum Frustum::FromViewProjection(const Matrix4& vp)
{
    Frustum f;

    /// @note left: row3 + row0
    f.SetPlane(0, Plane{
        { vp.m[3][0] + vp.m[0][0], vp.m[3][1] + vp.m[0][1], vp.m[3][2] + vp.m[0][2] },
          vp.m[3][3] + vp.m[0][3]
    }.Normalized());

    /// @note right: row3 - row0
    f.SetPlane(1, Plane{
        { vp.m[3][0] - vp.m[0][0], vp.m[3][1] - vp.m[0][1], vp.m[3][2] - vp.m[0][2] },
          vp.m[3][3] - vp.m[0][3]
    }.Normalized());

    /// @note bottom: row3 + row1
    f.SetPlane(2, Plane{
        { vp.m[3][0] + vp.m[1][0], vp.m[3][1] + vp.m[1][1], vp.m[3][2] + vp.m[1][2] },
          vp.m[3][3] + vp.m[1][3]
    }.Normalized());

    /// @note top: row3 - row1
    f.SetPlane(3, Plane{
        { vp.m[3][0] - vp.m[1][0], vp.m[3][1] - vp.m[1][1], vp.m[3][2] - vp.m[1][2] },
          vp.m[3][3] - vp.m[1][3]
    }.Normalized());

    /// @note near: row2 (DirectX 深度 [0,1]: clip.z >= 0)
    f.SetPlane(4, Plane{
        { vp.m[2][0], vp.m[2][1], vp.m[2][2] },
          vp.m[2][3]
    }.Normalized());

    /// @note far: row3 - row2
    f.SetPlane(5, Plane{
        { vp.m[3][0] - vp.m[2][0], vp.m[3][1] - vp.m[2][1], vp.m[3][2] - vp.m[2][2] },
          vp.m[3][3] - vp.m[2][3]
    }.Normalized());

    return f;
}

} // namespace fbzz::math
