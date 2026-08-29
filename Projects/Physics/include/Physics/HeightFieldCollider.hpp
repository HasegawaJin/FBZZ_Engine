/// @file    HeightFieldCollider.hpp
/// @brief   ハイトマップ地形向け最適化コライダー。
/// @author  Hasegawa Jin
/// @date    2026-06-16
///
/// TriangleMeshCollider の代替で、heightData をグリッドとして保持し
/// メモリ量を削減しつつ地形専用の BVH 再構築パスを提供する。
/// WHY: TriangleMeshCollider では positions + indices + BVH を保持するが、
/// 地形は均一グリッドのため float 配列だけで同等情報を表現できる。
/// また transform 変化と heightData 変化を分離して管理できる。
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
    // heights: row-major、値域 [-1, 1]、index = z * cols + x
    // ワールド高さ = heights[i] * maxHeight
    HeightFieldCollider(const std::vector<float>& heights,
                        int rows, int cols,
                        float cellSize, float maxHeight);

    AABB         GetAABB() const override { return m_worldAABB; }
    ColliderType GetType() const override { return ColliderType::HEIGHT_FIELD; }

    // Transform を BVH に反映する。heightData が変わっていなければ AABB 再計算のみ行い BVH を使い回す。
    // WHY: 地形は静的が前提。Transform 変化のたびに BVH を全再構築すると
    //      TriangleMeshCollider と同コストになるため、transform 変化は AABB だけ更新する。
    void Update(const math::Vector3& worldPos,
                const math::Quaternion& worldRot) override;
    void UpdateWithScale(const math::Vector3&    worldPos,
                         const math::Quaternion& worldRot,
                         const math::Vector3&    worldScale);

    // heightData が変更されたとき（地形彫刻後）に呼ぶ。BVH をフル再構築する。
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

    // ローカル空間の高さデータ（float のみ保持し positions/indices バッファを持たない）
    std::vector<float> m_heights;

    BVHTree m_bvh;
    AABB    m_worldAABB;

    math::Vector3    m_worldPos;
    math::Quaternion m_worldRot;
    math::Vector3    m_worldScale = { 1.f, 1.f, 1.f };

    // 現在の transform を使って BVH をフル再構築する
    void RebuildBVH();
    // BVH の世界座標 AABB を再計算する（BVH 再構築なし）
    AABB ComputeWorldAABB() const;
};

} // namespace fbzz::physics
