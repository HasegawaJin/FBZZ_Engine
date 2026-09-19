/// @file    Frustum.hpp
/// @brief   視錐台 (6平面による凸包)。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#pragma once

#include "Plane.hpp"
#include "Vector3.hpp"
#include "Matrix4.hpp"
#include "Simd.hpp"

#include <cstdint>
#include <span>

/// @note Plane.hpp include 後に別ヘッダ経由で Plane マクロが再定義された場合に備える。
///       Frustum は Plane 型を返すため、ここで名前を必ず型として解決させる。
#ifdef Plane
#undef Plane
#endif

namespace fbzz::math {

/// @note 6 平面を成分ごとの配列 (SoA) で持ち、4 平面ずつ 1 命令で判定する。書き込みは生成関数に閉じる。
/// @see Docs/design/math-simd.md §4 段 3a
/// @see https://fgiesen.wordpress.com/2010/10/17/view-frustum-culling/ Fabian Giesen «View frustum culling»
struct Frustum {
    static constexpr int PLANE_COUNT = 6;

    /// @brief 6 平面とも既定の Plane (法線 +Y・距離 0) で埋める。
    Frustum();

    /// @brief i 番目の平面 (left/right/bottom/top/near/far の順)。
    /// @pre 0 <= i < PLANE_COUNT。
    Plane GetPlane(int i) const;

    /// @brief 点が視錐台内にあるかを判定する。
    bool Contains(const Vector3& point) const;

    /// @brief 球が視錐台と交差するかを判定する。
    /// @return 完全に外側なら false。
    bool IntersectsSphere(const Vector3& center, float radius) const;

    /// @brief AABB が視錐台と交差するかを判定する。
    /// @return 完全に外側なら false。
    bool IntersectsAABB(const Vector3& center, const Vector3& halfExtents) const;

    /// @brief 多数の球をまとめて判定する。結果は IntersectsSphere を 1 個ずつ呼んだ場合と一致する。
    /// @param spheres xyz = 中心、w = 半径。
    /// @param visible 球ごとに交差なら 1、完全に外側なら 0 を書く。
    /// @pre visible.size() >= spheres.size()。足りなければ契約違反を報告し、入る数だけ判定する。
    /// @note 4 球ずつ転置して 6 平面と比べる。物体ごとに IntersectsSphere を呼ぶより 1 球あたりが安い。
    void IntersectsSpheres(std::span<const Vector4> spheres, std::span<uint8_t> visible) const;

    /// @brief ビュープロジェクション行列から 6 平面を抽出する (Gribb-Hartmann 法)。
    /// @pre DirectX 左手系、深度 [0,1]、列ベクトル規則 (M * v)。
    static Frustum FromViewProjection(const Matrix4& vp);

private:
    /// @brief 4 レーン × 2 本。レーン 6, 7 は使わず、判定のビットマスクで捨てる。
    static constexpr int LANE_COUNT = 8;
    static constexpr int PLANE_MASK = (1 << PLANE_COUNT) - 1;

    void SetPlane(int i, const Plane& plane);

    /// @brief 4 平面ぶんの符号付き距離 dot(n, p) + d。加算の順序は Plane::SignedDistanceTo と同じ。
    simd::Vec SignedDistances(int lane, simd::Vec x, simd::Vec y, simd::Vec z) const;

    alignas(16) float m_normalX[LANE_COUNT]  = {};
    alignas(16) float m_normalY[LANE_COUNT]  = {};
    alignas(16) float m_normalZ[LANE_COUNT]  = {};
    alignas(16) float m_distance[LANE_COUNT] = {};
};

inline Frustum::Frustum()
{
    for (int i = 0; i < PLANE_COUNT; ++i) SetPlane(i, Plane{});
}

inline Plane Frustum::GetPlane(int i) const
{
    return Plane{ { m_normalX[i], m_normalY[i], m_normalZ[i] }, m_distance[i] };
}

inline void Frustum::SetPlane(int i, const Plane& plane)
{
    m_normalX[i]  = plane.normal.x;
    m_normalY[i]  = plane.normal.y;
    m_normalZ[i]  = plane.normal.z;
    m_distance[i] = plane.distance;
}

/// @note FBZZMath は DLL のため、.cpp に置くと呼び出しごとに DLL 境界を越えてインライン化されない。カリングで物体ごとに呼ぶためヘッダーで定義する。
inline simd::Vec Frustum::SignedDistances(int lane, simd::Vec x, simd::Vec y, simd::Vec z) const
{
    simd::Vec distance = _mm_mul_ps(_mm_load_ps(m_normalX + lane), x);
    distance = simd::MulAdd(_mm_load_ps(m_normalY + lane), y, distance);
    distance = simd::MulAdd(_mm_load_ps(m_normalZ + lane), z, distance);
    return _mm_add_ps(distance, _mm_load_ps(m_distance + lane));
}

inline bool Frustum::Contains(const Vector3& point) const
{
    const simd::Vec x = _mm_set1_ps(point.x);
    const simd::Vec y = _mm_set1_ps(point.y);
    const simd::Vec z = _mm_set1_ps(point.z);
    const simd::Vec zero = _mm_setzero_ps();
    /// @note «>= 0 でない» で外側とする。NaN は外側に倒れ、スカラー版の !IsOnPositiveSide と同じになる。
    const int outside = _mm_movemask_ps(_mm_cmpnge_ps(SignedDistances(0, x, y, z), zero))
                      | _mm_movemask_ps(_mm_cmpnge_ps(SignedDistances(4, x, y, z), zero)) << 4;
    return (outside & PLANE_MASK) == 0;
}

inline bool Frustum::IntersectsSphere(const Vector3& center, float radius) const
{
    const simd::Vec x = _mm_set1_ps(center.x);
    const simd::Vec y = _mm_set1_ps(center.y);
    const simd::Vec z = _mm_set1_ps(center.z);
    const simd::Vec negRadius = _mm_set1_ps(-radius);
    const int outside = _mm_movemask_ps(_mm_cmplt_ps(SignedDistances(0, x, y, z), negRadius))
                      | _mm_movemask_ps(_mm_cmplt_ps(SignedDistances(4, x, y, z), negRadius)) << 4;
    return (outside & PLANE_MASK) == 0;
}

inline bool Frustum::IntersectsAABB(const Vector3& center, const Vector3& halfExtents) const
{
    const simd::Vec x = _mm_set1_ps(center.x);
    const simd::Vec y = _mm_set1_ps(center.y);
    const simd::Vec z = _mm_set1_ps(center.z);
    const simd::Vec hx = _mm_set1_ps(halfExtents.x);
    const simd::Vec hy = _mm_set1_ps(halfExtents.y);
    const simd::Vec hz = _mm_set1_ps(halfExtents.z);
    const simd::Vec signBit = _mm_set1_ps(-0.0f);
    int outside = 0;
    for (int lane = 0; lane < LANE_COUNT; lane += 4) {
        /// @note 各軸の half-extent を法線方向に投影した最大値 (法線の絶対値との内積)。
        simd::Vec reach = _mm_mul_ps(hx, _mm_andnot_ps(signBit, _mm_load_ps(m_normalX + lane)));
        reach = simd::MulAdd(hy, _mm_andnot_ps(signBit, _mm_load_ps(m_normalY + lane)), reach);
        reach = simd::MulAdd(hz, _mm_andnot_ps(signBit, _mm_load_ps(m_normalZ + lane)), reach);
        const simd::Vec negReach = _mm_xor_ps(reach, signBit);
        outside |= _mm_movemask_ps(_mm_cmplt_ps(SignedDistances(lane, x, y, z), negReach)) << lane;
    }
    return (outside & PLANE_MASK) == 0;
}

} // namespace fbzz::math
