/// @file    ColliderDebugGeometry.cpp
/// @brief   コライダーの形状と視点から、デバッグ線分の生成方法を選ぶ。
/// @author  Hasegawa Jin
/// @date    2026-05-24

#include <Physics/ColliderDebugGeometry.hpp>
#include "ColliderDebugGeometryDetail.hpp"
#include <Physics/CapsuleCollider.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/CylinderCollider.hpp>
#include <Physics/HeightFieldCollider.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>

namespace fbzz::physics
{
    namespace
    {
        const BVHTree* FindMeshBVH(const Collider& collider)
        {
            switch (collider.GetType())
            {
            case ColliderType::TRIANGLE_MESH:
                return &static_cast<const TriangleMeshCollider&>(collider).GetBVH();
            case ColliderType::HEIGHT_FIELD:
                return &static_cast<const HeightFieldCollider&>(collider).GetBVH();
            default:
                return nullptr;
            }
        }

        ColliderDebugGeometry BuildShapeGeometry(const Collider& collider)
        {
            switch (collider.GetType())
            {
            case ColliderType::SPHERE:
                return collider_debug_detail::BuildSphereGeometry(static_cast<const SphereCollider&>(collider));
            case ColliderType::AABB:
                return collider_debug_detail::BuildBoundsGeometry(collider);
            case ColliderType::OBB:
                return collider_debug_detail::BuildOBBGeometry(static_cast<const OBBCollider&>(collider));
            case ColliderType::CAPSULE:
                return collider_debug_detail::BuildCapsuleGeometry(static_cast<const CapsuleCollider&>(collider));
            case ColliderType::CYLINDER:
                return collider_debug_detail::BuildCylinderGeometry(static_cast<const CylinderCollider&>(collider));
            case ColliderType::TRIANGLE_MESH:
                return collider_debug_detail::BuildMeshGeometry(
                    static_cast<const TriangleMeshCollider&>(collider).GetBVH().triangles);
            case ColliderType::CONVEX_HULL:
                return collider_debug_detail::BuildConvexHullGeometry(static_cast<const ConvexHullCollider&>(collider));
            case ColliderType::HEIGHT_FIELD:
                return collider_debug_detail::BuildMeshGeometry(
                    static_cast<const HeightFieldCollider&>(collider).GetBVH().triangles);
            }

            return collider_debug_detail::BuildBoundsGeometry(collider);
        }
    }

    ColliderDebugGeometry BuildColliderDebugGeometry(const Collider& collider)
    {
        ColliderDebugGeometry geometry = BuildShapeGeometry(collider);
        geometry.detailLineCount = geometry.lines.size();
        return geometry;
    }

    ColliderDebugGeometry BuildColliderDebugGeometry(const Collider& collider, const ColliderDebugView& view)
    {
        if (view.enabled && view.detailRadius > 0.0f)
        {
            if (const BVHTree* bvh = FindMeshBVH(collider))
                return collider_debug_detail::BuildBVHLodGeometry(*bvh, view);
        }
        return BuildColliderDebugGeometry(collider);
    }
}
