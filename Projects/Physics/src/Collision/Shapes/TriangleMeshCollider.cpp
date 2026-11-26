/// @file    TriangleMeshCollider.cpp
/// @brief   メッシュコライダーの姿勢変更に応じて BVH の更新方法を選ぶ。
/// @author  Hasegawa Jin
/// @date    2026-05-24

#include <Physics/Collision/Shapes/TriangleMeshCollider.hpp>
#include <cmath>

namespace fbzz::physics
{
    void TriangleMeshCollider::Update(const math::Vector3& worldPosition,
                                       const math::Quaternion& worldRotation)
    {
        UpdateWithScale(worldPosition, worldRotation, m_lastScale);
    }

    void TriangleMeshCollider::UpdateWithScale(const math::Vector3& worldPosition,
                                                const math::Quaternion& worldRotation,
                                                const math::Vector3& worldScale)
    {
        constexpr float TRANSFORM_TOLERANCE = 1e-5f;
        constexpr float TRANSFORM_TOLERANCE_SQUARED = TRANSFORM_TOLERANCE * TRANSFORM_TOLERANCE;
        const bool positionChanged = (worldPosition - m_lastPos).LengthSq() > TRANSFORM_TOLERANCE_SQUARED;
        const bool scaleChanged = (worldScale - m_lastScale).LengthSq() > TRANSFORM_TOLERANCE_SQUARED;
        // q と −q は同じ回転を表す。
        const float rotationAlignment = std::abs(math::Quaternion::Dot(worldRotation, m_lastRot));
        const bool rotationChanged = rotationAlignment < (1.0f - TRANSFORM_TOLERANCE);

        if (!positionChanged && !rotationChanged && !scaleChanged) {
            return;
        }

        // 剛体変換では三角形の所属する葉を保持する。スケール変更は縮退面の集合も変えうる。
        if (!scaleChanged && !m_bvh.triangles.empty()) {
            RefitTransform(worldPosition, worldRotation);
        }
        else {
            RebuildBVH(worldPosition, worldRotation, worldScale);
        }
    }
}
