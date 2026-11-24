/// @file    TriangleMeshGeometry.cpp
/// @brief   ローカルの頂点・添字から BVH に渡すワールド三角形を準備する。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/TriangleMeshCollider.hpp>
#include <cassert>
#include <cstddef>
#include <utility>

namespace fbzz::physics
{
    namespace
    {
        constexpr std::size_t VERTICES_PER_TRIANGLE = 3;
        constexpr float MIN_FACE_NORMAL_LENGTH_SQUARED = 1e-10f;

        math::Vector3 TransformVertex(const math::Vector3& localPosition,
                                      const math::Vector3& worldPosition,
                                      const math::Quaternion& worldRotation,
                                      const math::Vector3& worldScale)
        {
            const math::Vector3 scaledPosition = {localPosition.x * worldScale.x,
                                                  localPosition.y * worldScale.y,
                                                  localPosition.z * worldScale.z};
            return worldPosition + worldRotation * scaledPosition;
        }

        math::Vector3 UnnormalizedFaceNormal(const Triangle& triangle)
        {
            return math::Vector3::Cross(triangle.v[1] - triangle.v[0], triangle.v[2] - triangle.v[0]);
        }
    }

    TriangleMeshCollider::TriangleMeshCollider(const std::vector<math::Vector3>& positions,
                                              const std::vector<uint32_t>& indices)
        : m_localPositions(positions)
        , m_indices(indices)
    {
        /// @note インポートや手続き生成の入力にある不完全な末尾は、三角形にせず捨てる。
        m_indices.resize(m_indices.size() - m_indices.size() % VERTICES_PER_TRIANGLE);
        RebuildBVH(math::Vector3::ZERO, math::Quaternion::Identity(), {1.0f, 1.0f, 1.0f});
    }

    void TriangleMeshCollider::RebuildBVH(const math::Vector3& worldPosition,
                                          const math::Quaternion& worldRotation,
                                          const math::Vector3& worldScale)
    {
        m_lastPos = worldPosition;
        m_lastRot = worldRotation;
        m_lastScale = worldScale;

        const std::size_t inputTriangleCount = m_indices.size() / VERTICES_PER_TRIANGLE;
        std::vector<Triangle> worldTriangles;
        worldTriangles.reserve(inputTriangleCount);
        m_worldAABB = mesh_bvh_detail::MakeEmptyBounds();
        for (std::size_t inputTriangleIndex = 0; inputTriangleIndex < inputTriangleCount; ++inputTriangleIndex)
        {
            Triangle triangle;
            triangle.index = static_cast<uint32_t>(inputTriangleIndex);
            const std::size_t firstIndex = inputTriangleIndex * VERTICES_PER_TRIANGLE;
            for (std::size_t cornerIndex = 0; cornerIndex < VERTICES_PER_TRIANGLE; ++cornerIndex)
            {
                const uint32_t vertexIndex = m_indices[firstIndex + cornerIndex];
                assert(vertexIndex < m_localPositions.size());
                triangle.v[cornerIndex] =
                    TransformVertex(m_localPositions[vertexIndex], worldPosition, worldRotation, worldScale);
                /// @note 再構築時の外接箱には、BVH から除外する縮退三角形の頂点も含める。
                mesh_bvh_detail::IncludePoint(m_worldAABB, triangle.v[cornerIndex]);
            }

            const math::Vector3 faceNormal = UnnormalizedFaceNormal(triangle);
            if (faceNormal.LengthSq() < MIN_FACE_NORMAL_LENGTH_SQUARED) continue;

            triangle.normal = faceNormal.Normalized();
            worldTriangles.push_back(triangle);
        }
        m_bvh.Build(std::move(worldTriangles));
    }

    void TriangleMeshCollider::RefitTransform(const math::Vector3& worldPosition,
                                             const math::Quaternion& worldRotation)
    {
        m_lastPos = worldPosition;
        m_lastRot = worldRotation;

        for (Triangle& triangle : m_bvh.triangles)
        {
            /// @note 縮退面を除外しても残した入力上の番号で、元のローカル頂点を引き直す。
            const std::size_t firstIndex = static_cast<std::size_t>(triangle.index) * VERTICES_PER_TRIANGLE;
            if (firstIndex + VERTICES_PER_TRIANGLE > m_indices.size()) continue;

            for (std::size_t cornerIndex = 0; cornerIndex < VERTICES_PER_TRIANGLE; ++cornerIndex)
            {
                const uint32_t vertexIndex = m_indices[firstIndex + cornerIndex];
                if (vertexIndex >= m_localPositions.size()) continue;
                triangle.v[cornerIndex] =
                    TransformVertex(m_localPositions[vertexIndex], worldPosition, worldRotation, m_lastScale);
            }

            /// @note 頂点から法線を引き直し、前回の回転の保持順に依存させない。
            const math::Vector3 faceNormal = UnnormalizedFaceNormal(triangle);
            if (faceNormal.LengthSq() >= MIN_FACE_NORMAL_LENGTH_SQUARED)
                triangle.normal = faceNormal.Normalized();
        }

        m_bvh.Refit();
        m_worldAABB = m_bvh.nodes.empty() ? AABB{} : m_bvh.nodes.front().aabb;
    }
}
