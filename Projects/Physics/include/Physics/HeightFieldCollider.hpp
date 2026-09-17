/// @file    HeightFieldCollider.hpp
/// @brief   ハイトマップ地形向け最適化コライダー。
/// @author  Hasegawa Jin
/// @date    2026-06-16
///
/// @note TriangleMeshCollider の代替。heightData をグリッドとして保持し、メモリ量を
///       削減しつつ地形専用の BVH 再構築パスを提供する。地形は均一グリッドのため
///       float 配列だけで positions + indices + BVH と同等の情報を表現でき、
///       transform 変化と heightData 変化を分離して管理できる。
#pragma once
#include <Physics/Collider.hpp>
#include <Physics/BVHNode.hpp>
#include <Math/Vector3.hpp>
#include <vector>

namespace fbzz::physics
{

class HeightFieldCollider : public Collider
{
public:
    /// @param heights row-major、値域 [-1, 1]、index = z * cols + x。ワールド高さ = heights[i] * maxHeight。
    HeightFieldCollider(const std::vector<float>& heights,
                        int rows, int cols,
                        float cellSize, float maxHeight);

    AABB         GetAABB() const override { return m_worldAABB; }
    ColliderType GetType() const override { return ColliderType::HEIGHT_FIELD; }

    /// @brief Transform を BVH に反映する。
    /// @note heightData が変わっていなければ木の構造は使い回し、三角形の頂点を置き直して
    ///       ノード AABB を refit するだけで済ませる。地形は静的が前提だが、エディタで
    ///       ギズモを掴めば動く。毎回全再構築すると TriangleMeshCollider と同コストになる。
    void Update(const math::Vector3& worldPos,
                const math::Quaternion& worldRot) override;
    void UpdateWithScale(const math::Vector3&    worldPos,
                         const math::Quaternion& worldRot,
                         const math::Vector3&    worldScale);

    /// @brief heightData が変更されたとき (地形彫刻後) に呼ぶ。BVH をフル再構築する。
    void Rebuild(const std::vector<float>& heights,
                 int rows, int cols,
                 float cellSize, float maxHeight);

    const BVHTree& GetBVH() const { return m_bvh; }

    int   GetRows()      const { return m_rows; }
    int   GetCols()      const { return m_cols; }
    float GetCellSize()  const { return m_cellSize; }
    float GetMaxHeight() const { return m_maxHeight; }

private:
    int   m_rows      = 0;
    int   m_cols      = 0;
    float m_cellSize  = 1.0f;
    float m_maxHeight = 1.0f;

    std::vector<float> m_heights;  ///< ローカル空間の高さデータ (positions/indices バッファは持たない)

    BVHTree m_bvh;
    AABB    m_worldAABB;

    math::Vector3    m_worldPos;
    math::Quaternion m_worldRot;
    math::Vector3    m_worldScale = { 1.f, 1.f, 1.f };

    /// @brief 格子点 (x, z) のローカル座標。ワールド高さ = heights[i] * maxHeight。
    math::Vector3 LocalVertex(int x, int z) const;
    /// @brief ローカル座標へ scale → rotation → translation を掛ける。
    math::Vector3 ToWorld(const math::Vector3& local) const;

    /// @brief 現在の transform を使って BVH をフル再構築する。
    void RebuildBVH();
    /// @brief 木の構造はそのままに、現在の transform で三角形を置き直して AABB を refit する。
    void RefitTransform();
};

} // namespace fbzz::physics
