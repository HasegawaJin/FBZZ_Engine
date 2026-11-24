/// @file    HeightFieldCollider.cpp
/// @brief   高さ場の入力の保持と、変更内容に応じた BVH 更新の選択。
/// @author  Hasegawa Jin
/// @date    2026-06-16

#include <Physics/Collision/Shapes/HeightFieldCollider.hpp>
#include <cassert>
#include <cmath>
#include <utility>

namespace fbzz::physics
{

HeightFieldCollider::HeightFieldCollider(const std::vector<float>& heightSamples,
                                        int rowCount, int columnCount,
                                        float cellSize, float maxHeight,
                                        std::vector<std::uint8_t> holeMask)
    : m_rowCount(rowCount)
    , m_columnCount(columnCount)
    , m_cellSize(cellSize)
    , m_maxHeight(maxHeight)
    , m_heightSamples(heightSamples)
    , m_holeMask(std::move(holeMask))
{
    assert(rowCount >= 2 && columnCount >= 2);
    assert(static_cast<int>(heightSamples.size()) == rowCount * columnCount);
    SanitizeHoles();
    /// @note ワールド三角形は最初に姿勢が与えられたときに構築する。
}

void HeightFieldCollider::Update(const math::Vector3& worldPosition,
                                 const math::Quaternion& worldRotation)
{
    UpdateWithScale(worldPosition, worldRotation, m_worldScale);
}

void HeightFieldCollider::UpdateWithScale(const math::Vector3& worldPosition,
                                          const math::Quaternion& worldRotation,
                                          const math::Vector3& worldScale)
{
    constexpr float TRANSFORM_TOLERANCE = 1e-5f;
    constexpr float TRANSFORM_TOLERANCE_SQUARED = TRANSFORM_TOLERANCE * TRANSFORM_TOLERANCE;
    const bool positionChanged = (worldPosition - m_worldPosition).LengthSq() > TRANSFORM_TOLERANCE_SQUARED;
    const bool scaleChanged = (worldScale - m_worldScale).LengthSq() > TRANSFORM_TOLERANCE_SQUARED;
    const float rotationAlignment = std::abs(math::Quaternion::Dot(worldRotation, m_worldRotation));
    const bool rotationChanged = rotationAlignment < (1.0f - TRANSFORM_TOLERANCE);

    m_worldPosition = worldPosition;
    m_worldRotation = worldRotation;
    m_worldScale = worldScale;

    /// @note 初回は姿勢が既定値でも構築する。空の高さ場も現在の位置へ外接箱を同期する。
    if (m_bvh.triangles.empty())
    {
        RebuildBVH();
        return;
    }

    /// @note 高さと穴が同じなら、スケール変更でも三角形の所属する葉を保持する。
    if (positionChanged || rotationChanged || scaleChanged)
        RefitTransform();
}

void HeightFieldCollider::Rebuild(const std::vector<float>& heightSamples,
                                  int rowCount, int columnCount,
                                  float cellSize, float maxHeight,
                                  std::vector<std::uint8_t> holeMask)
{
    assert(rowCount >= 2 && columnCount >= 2);
    assert(static_cast<int>(heightSamples.size()) == rowCount * columnCount);
    m_rowCount = rowCount;
    m_columnCount = columnCount;
    m_cellSize = cellSize;
    m_maxHeight = maxHeight;
    m_heightSamples = heightSamples;
    m_holeMask = std::move(holeMask);
    SanitizeHoles();
    RebuildBVH();
}

} /// @note namespace fbzz::physics
