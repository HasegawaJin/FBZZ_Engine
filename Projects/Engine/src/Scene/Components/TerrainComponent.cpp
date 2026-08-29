/// @file    TerrainComponent.cpp
/// @brief   ハイトマップ地形コンポーネントのメソッド実装。
/// @author  Hasegawa Jin
/// @date    2026-05-31
///
/// ここでは純粋なデータ操作（高さクエリ・法線計算）のみを行う。
/// GPU メッシュ生成・描画は TerrainRenderPass.cpp に委譲する。
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include <Math/Vector3.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {

// -----------------------------------------------------------------------------
// ComputeNormal — 有限差分で 1 グリッド頂点の法線を計算する
// -----------------------------------------------------------------------------
// 境界外インデックスはクランプサンプリングで対応する。
//
//   dh/dx = (h(x+1, z) - h(x-1, z)) / (2 * cellSize)
//   dh/dz = (h(x, z+1) - h(x, z-1)) / (2 * cellSize)
//   normal = normalize( (-dh/dx, 1, -dh/dz) )   [Y 上向き座標系]
//
// WHY 中心差分: 片側差分より精度が高く、境界でも clamp サンプリングで安定する。
math::Vector3 TerrainComponent::ComputeNormal(int x, int z) const
{
    float dhdx = (SampleHeight(x + 1, z) - SampleHeight(x - 1, z)) / (2.0f * cellSize);
    float dhdz = (SampleHeight(x, z + 1) - SampleHeight(x, z - 1)) / (2.0f * cellSize);
    return math::Vector3{ -dhdx, 1.0f, -dhdz }.Normalized();
}

// -----------------------------------------------------------------------------
// GetHeightAt — 描画メッシュ・MeshCollider と同じ2三角形分割で高さを補間する
// WHY: バイリニア補間は非平面セル内で三角形面と一致せず、Detail や足 IK が地面から浮くため。
// -----------------------------------------------------------------------------
// 範囲外座標はクランプして継続する（assert せず、Physics・足 IK から呼ばれるため）。
float TerrainComponent::GetHeightAt(float localX, float localZ) const
{
    if (heightData.empty()) return 0.0f;

    const float gx = std::clamp(localX / cellSize, 0.0f, static_cast<float>(columns - 1));
    const float gz = std::clamp(localZ / cellSize, 0.0f, static_cast<float>(rows - 1));

    int   x0 = std::clamp(static_cast<int>(gx), 0, columns - 2);
    int   z0 = std::clamp(static_cast<int>(gz), 0, rows    - 2);
    float fx  = gx - static_cast<float>(x0);
    float fz  = gz - static_cast<float>(z0);

    auto h = [&](int xi, int zi) {
        return heightData[static_cast<size_t>(zi) * static_cast<size_t>(columns)
                        + static_cast<size_t>(xi)] * maxHeight;
    };

    float h00 = h(x0,     z0);
    float h10 = h(x0 + 1, z0);
    float h01 = h(x0,     z0 + 1);
    float h11 = h(x0 + 1, z0 + 1);

    // TerrainRenderPass / PhysicsSystem のインデックス分割:
    //   lower: i00, i01, i10 / upper: i10, i01, i11
    if (fx + fz <= 1.0f)
        return h00 + fx * (h10 - h00) + fz * (h01 - h00);

    return h10 * (1.0f - fz)
         + h01 * (1.0f - fx)
         + h11 * (fx + fz - 1.0f);
}

// -----------------------------------------------------------------------------
// GetNormalAt — ローカル座標から補間法線を返す
// -----------------------------------------------------------------------------
// 隣接グリッド頂点の法線をバイリニア補間して返す。
// 有限差分の局所的なノイズが補間によって滑らかになる効果もある。
math::Vector3 TerrainComponent::GetNormalAt(float localX, float localZ) const
{
    if (heightData.empty()) return { 0.0f, 1.0f, 0.0f };

    float gx = localX / cellSize;
    float gz = localZ / cellSize;

    int   x0 = std::clamp(static_cast<int>(gx), 0, columns - 2);
    int   z0 = std::clamp(static_cast<int>(gz), 0, rows    - 2);
    float fx  = gx - static_cast<float>(x0);
    float fz  = gz - static_cast<float>(z0);

    math::Vector3 n00 = ComputeNormal(x0,     z0);
    math::Vector3 n10 = ComputeNormal(x0 + 1, z0);
    math::Vector3 n01 = ComputeNormal(x0,     z0 + 1);
    math::Vector3 n11 = ComputeNormal(x0 + 1, z0 + 1);

    math::Vector3 n;
    n.x = n00.x * (1.0f - fx) * (1.0f - fz)
        + n10.x * fx          * (1.0f - fz)
        + n01.x * (1.0f - fx) * fz
        + n11.x * fx          * fz;
    n.y = n00.y * (1.0f - fx) * (1.0f - fz)
        + n10.y * fx          * (1.0f - fz)
        + n01.y * (1.0f - fx) * fz
        + n11.y * fx          * fz;
    n.z = n00.z * (1.0f - fx) * (1.0f - fz)
        + n10.z * fx          * (1.0f - fz)
        + n01.z * (1.0f - fx) * fz
        + n11.z * fx          * fz;
    return n.Normalized();
}

} // namespace fbzz::scene
