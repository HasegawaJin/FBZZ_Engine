/// @file    HeightFieldBVH.cpp
/// @brief   高さ場の BVH 構築と、姿勢変更時の境界の同期。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/Collision/Shapes/HeightFieldCollider.hpp>

namespace fbzz::physics
{

void HeightFieldCollider::RebuildBVH()
{
    if (m_heightSamples.empty() || m_rowCount < 2 || m_columnCount < 2) {
        return;
    }

    m_bvh.Build(BuildWorldTriangles());
    // 全面が穴または縮退面なら、外接箱を地形原点のワールド座標へ縮める。
    m_worldAABB = m_bvh.nodes.empty() ? AABB{m_worldPosition, m_worldPosition} : m_bvh.nodes.front().aabb;
}

void HeightFieldCollider::RefitTransform()
{
    if (m_bvh.triangles.empty() || m_heightSamples.empty() || m_rowCount < 2 || m_columnCount < 2) {
        return;
    }

    UpdateWorldTriangles(m_bvh.triangles);
    m_bvh.Refit();
    m_worldAABB = m_bvh.nodes.empty() ? AABB{} : m_bvh.nodes.front().aabb;
}

} /// @note namespace fbzz::physics
