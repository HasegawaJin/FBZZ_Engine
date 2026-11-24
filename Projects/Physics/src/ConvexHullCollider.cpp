/// @file    ConvexHullCollider.cpp
/// @brief   凸包コライダーの生成、ワールド座標と外接箱の更新。
/// @author  Hasegawa Jin
/// @date    2026-05-24

#include <Physics/ConvexHullCollider.hpp>
#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace fbzz::physics
{
    ConvexHullCollider::ConvexHullCollider(std::vector<math::Vector3> points)
    {
        BuildHull(std::move(points));
        UpdateWorldVerts(math::Vector3::ZERO, math::Quaternion::Identity(), {1.0f, 1.0f, 1.0f});
    }

    void ConvexHullCollider::Update(const math::Vector3& worldPosition,
                                    const math::Quaternion& worldRotation)
    {
        UpdateWithScale(worldPosition, worldRotation, m_worldScale);
    }

    void ConvexHullCollider::UpdateWithScale(const math::Vector3& worldPosition,
                                             const math::Quaternion& worldRotation,
                                             const math::Vector3& worldScale)
    {
        UpdateWorldVerts(worldPosition, worldRotation, worldScale);
    }

    void ConvexHullCollider::UpdateWorldVerts(const math::Vector3& worldPosition,
                                              const math::Quaternion& worldRotation,
                                              const math::Vector3& worldScale)
    {
        m_worldCenter = worldPosition;
        m_worldRot = worldRotation;
        m_worldScale = worldScale;
        m_worldVerts.resize(m_localVerts.size());

        constexpr float HIGHEST_COORDINATE = std::numeric_limits<float>::max();
        constexpr float LOWEST_COORDINATE = std::numeric_limits<float>::lowest();
        m_worldAABB.min = {HIGHEST_COORDINATE, HIGHEST_COORDINATE, HIGHEST_COORDINATE};
        m_worldAABB.max = {LOWEST_COORDINATE, LOWEST_COORDINATE, LOWEST_COORDINATE};

        for (std::size_t vertexIndex = 0; vertexIndex < m_localVerts.size(); ++vertexIndex)
        {
            const math::Vector3& localVertex = m_localVerts[vertexIndex];
            const math::Vector3 scaledVertex = {
                localVertex.x * worldScale.x,
                localVertex.y * worldScale.y,
                localVertex.z * worldScale.z
            };
            const math::Vector3 worldVertex = worldPosition + worldRotation * scaledVertex;
            m_worldVerts[vertexIndex] = worldVertex;

            m_worldAABB.min.x = std::min(m_worldAABB.min.x, worldVertex.x);
            m_worldAABB.min.y = std::min(m_worldAABB.min.y, worldVertex.y);
            m_worldAABB.min.z = std::min(m_worldAABB.min.z, worldVertex.z);
            m_worldAABB.max.x = std::max(m_worldAABB.max.x, worldVertex.x);
            m_worldAABB.max.y = std::max(m_worldAABB.max.y, worldVertex.y);
            m_worldAABB.max.z = std::max(m_worldAABB.max.z, worldVertex.z);
        }
    }
}
