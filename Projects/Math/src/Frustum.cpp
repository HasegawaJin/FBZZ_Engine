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
constexpr std::array<uint32_t, 16> VISIBLE_BYTES_BY_OUTSIDE_MASK = [] {
    std::array<uint32_t, 16> table{};
    for (uint32_t mask = 0; mask < table.size(); ++mask) {
        for (uint32_t lane = 0; lane < 4; ++lane) {
            if ((mask & (1u << lane)) == 0) {
                table[mask] |= 1u << (lane * 8);
            }
        }
    }
    return table;
}();

Plane PlaneFromCoefficients(const Vector4& coefficients)
{
    return {coefficients.XYZ(), coefficients.w};
}

/// @note 投影行列から取り出した遠平面の法線は near / (far - near) 程度まで小さくなる。
/// @note 平面の大きさに関わらず、カリングで球や AABB の半径と比べる距離はワールド単位に揃える。
Plane NormalizeDepthPlane(const Plane& plane)
{
    const float length = plane.normal.Length();
    if (length == 0.0f) {
        return plane;
    }
    const float inverseLength = 1.0f / length;
    return { plane.normal * inverseLength, plane.distance * inverseLength };
}

}

/// @note 4 球を転置して «球がレーン・平面がループ» にする。単体版は «平面がレーン» なので、判定の式と加算の順序は同じで結果も一致する。
void Frustum::IntersectsSpheres(std::span<const Vector4> spheres, std::span<uint8_t> visible) const
{
    FBZZ_MATH_CONTRACT(visible.size() >= spheres.size(),
                       "visible is shorter than spheres; judging only what fits");
    const size_t count = std::min(spheres.size(), visible.size());

    /// @note 平面の係数を 4 レーンへ複製して置く。24 本をレジスタ (16 本) に抱えたままにはできないので、ループ内は L1 から読む。
    simd::Vec planeNormalX[PLANE_COUNT];
    simd::Vec planeNormalY[PLANE_COUNT];
    simd::Vec planeNormalZ[PLANE_COUNT];
    simd::Vec planeOffset[PLANE_COUNT];
    for (int plane = 0; plane < PLANE_COUNT; ++plane) {
        planeNormalX[plane] = _mm_set1_ps(m_normalX[plane]);
        planeNormalY[plane] = _mm_set1_ps(m_normalY[plane]);
        planeNormalZ[plane] = _mm_set1_ps(m_normalZ[plane]);
        planeOffset[plane] = _mm_set1_ps(m_distance[plane]);
    }

    const simd::Vec signBit = _mm_set1_ps(-0.0f);
    size_t sphere = 0;
    for (; sphere + 4 <= count; sphere += 4) {
        simd::Vec centerX = simd::Load4(&spheres[sphere].x);
        simd::Vec centerY = simd::Load4(&spheres[sphere + 1].x);
        simd::Vec centerZ = simd::Load4(&spheres[sphere + 2].x);
        simd::Vec radius = simd::Load4(&spheres[sphere + 3].x);
        simd::Transpose4x4(centerX, centerY, centerZ, radius);
        const simd::Vec negativeRadius = _mm_xor_ps(radius, signBit);

        simd::Vec outside = _mm_setzero_ps();
        for (int plane = 0; plane < PLANE_COUNT; ++plane) {
            simd::Vec signedDistance = _mm_mul_ps(planeNormalX[plane], centerX);
            signedDistance = simd::MulAdd(planeNormalY[plane], centerY, signedDistance);
            signedDistance = simd::MulAdd(planeNormalZ[plane], centerZ, signedDistance);
            signedDistance = _mm_add_ps(signedDistance, planeOffset[plane]);
            outside = _mm_or_ps(outside, _mm_cmplt_ps(signedDistance, negativeRadius));
        }
        const uint32_t bytes = VISIBLE_BYTES_BY_OUTSIDE_MASK[_mm_movemask_ps(outside)];
        std::memcpy(&visible[sphere], &bytes, sizeof(bytes));
    }
    for (; sphere < count; ++sphere) {
        visible[sphere] = IntersectsSphere(spheres[sphere].XYZ(), spheres[sphere].w) ? 1 : 0;
    }
}

/// @note Gribb-Hartmann 法。M * v (列ベクトル規則) では clip.i = dot(row_i, world)。
/// @note DirectX の深度は [0,1] なので、z = 0 側は clip.z の行だけで求める。
Frustum Frustum::FromViewProjection(const Matrix4& viewProjection)
{
    const auto clipRow = [&viewProjection](int index) {
        const auto& row = viewProjection.m[index];
        return Vector4{row[0], row[1], row[2], row[3]};
    };
    const Vector4 clipX = clipRow(0);
    const Vector4 clipY = clipRow(1);
    const Vector4 clipZ = clipRow(2);
    const Vector4 clipW = clipRow(3);

    Frustum result;
    result.SetPlane(0, PlaneFromCoefficients(clipW + clipX).Normalized());
    result.SetPlane(1, PlaneFromCoefficients(clipW - clipX).Normalized());
    result.SetPlane(2, PlaneFromCoefficients(clipW + clipY).Normalized());
    result.SetPlane(3, PlaneFromCoefficients(clipW - clipY).Normalized());
    result.SetPlane(4, NormalizeDepthPlane(PlaneFromCoefficients(clipZ)));
    result.SetPlane(5, NormalizeDepthPlane(PlaneFromCoefficients(clipW - clipZ)));
    return result;
}

}
