/// @file    PhysicsSolver.cpp
/// @brief   形状ペアの詳細衝突判定と接触マニフォールドの生成。
/// @author  Hasegawa Jin
/// @date    2026-05-21

#include <Physics/PhysicsSolver.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace fbzz::physics
{
    namespace
    {
        struct ContactManifold
        {
            ContactPoint points[4];
            int count = 0;
        };

        void AddManifoldPoint(ContactManifold& manifold, const ContactPoint& base,
                              const math::Vector3& point)
        {
            ContactPoint cp = base;
            cp.point = point;
            manifold.points[manifold.count++] = cp;
        }

        ContactManifold BuildAABBManifold(const AABB& aabbA, const AABB& aabbB,
                                          const ContactPoint& base)
        {
            constexpr float EPS = 1e-5f;

            ContactManifold manifold;
            const float minX = std::max(aabbA.min.x, aabbB.min.x);
            const float maxX = std::min(aabbA.max.x, aabbB.max.x);
            const float minY = std::max(aabbA.min.y, aabbB.min.y);
            const float maxY = std::min(aabbA.max.y, aabbB.max.y);
            const float minZ = std::max(aabbA.min.z, aabbB.min.z);
            const float maxZ = std::min(aabbA.max.z, aabbB.max.z);

            auto AddRect = [&](float fixed, int axis)
            {
                float u0 = minX;
                float u1 = maxX;
                float v0 = minZ;
                float v1 = maxZ;

                if (axis == 0)
                {
                    u0 = minY;
                    u1 = maxY;
                }
                else if (axis == 2)
                {
                    v0 = minY;
                    v1 = maxY;
                }

                if ((u1 - u0) <= EPS || (v1 - v0) <= EPS)
                {
                    AddManifoldPoint(manifold, base, base.point);
                    return;
                }

                const float us[2] = { u0, u1 };
                const float vs[2] = { v0, v1 };
                for (float u : us)
                {
                    for (float v : vs)
                    {
                        if (axis == 0)
                            AddManifoldPoint(manifold, base, { fixed, u, v });
                        else if (axis == 1)
                            AddManifoldPoint(manifold, base, { u, fixed, v });
                        else
                            AddManifoldPoint(manifold, base, { u, v, fixed });
                    }
                }
            };

            if (std::abs(base.normal.x) > 0.5f)
                AddRect(base.point.x, 0);
            else if (std::abs(base.normal.y) > 0.5f)
                AddRect(base.point.y, 1);
            else
                AddRect(base.point.z, 2);

            if (manifold.count == 0)
                AddManifoldPoint(manifold, base, base.point);

            const float weight = 1.0f / static_cast<float>(manifold.count);
            for (int i = 0; i < manifold.count; ++i)
                manifold.points[i].positionCorrectionWeight = weight;

            return manifold;
        }

        bool IsPointInsideOBB(const math::Vector3& point, const OBBCollider& box, float tolerance)
        {
            const math::Vector3 delta = point - box.GetCenter();
            for (int i = 0; i < 3; ++i)
            {
                const float extent = i == 0 ? box.m_halfExtents.x : (i == 1 ? box.m_halfExtents.y : box.m_halfExtents.z);
                if (std::abs(math::Vector3::Dot(delta, box.GetAxis(i))) > extent + tolerance)
                    return false;
            }
            return true;
        }

        void AddUniqueManifoldPoint(ContactManifold& manifold, const ContactPoint& base,
                                    const math::Vector3& point)
        {
            constexpr float MATCH_RADIUS_SQ = 1e-4f;
            for (int i = 0; i < manifold.count; ++i)
            {
                if ((manifold.points[i].point - point).LengthSq() <= MATCH_RADIUS_SQ)
                    return;
            }

            if (manifold.count < 4)
                AddManifoldPoint(manifold, base, point);
        }

        ContactManifold BuildOBBManifold(const OBBCollider& a, const OBBCollider& b,
                                         const ContactPoint& base)
        {
            ContactManifold manifold;
            const float tolerance = std::max(base.depth, 0.01f) + 1e-4f;
            const auto cornersA = a.GetCorners();
            const auto cornersB = b.GetCorners();

            for (const math::Vector3& point : cornersA)
            {
                if (IsPointInsideOBB(point, b, tolerance))
                    AddUniqueManifoldPoint(manifold, base, point);
            }
            for (const math::Vector3& point : cornersB)
            {
                if (IsPointInsideOBB(point, a, tolerance))
                    AddUniqueManifoldPoint(manifold, base, point);
            }

            if (manifold.count == 0)
                AddManifoldPoint(manifold, base, base.point);

            const float weight = 1.0f / static_cast<float>(manifold.count);
            for (int i = 0; i < manifold.count; ++i)
                manifold.points[i].positionCorrectionWeight = weight;

            return manifold;
        }

        /// @note 判定関数が期待する引数順。メッシュと高さ場は凸包と組む場合も B 側に置く。
        int ContactShapeOrder(ColliderType type)
        {
            switch (type)
            {
            case ColliderType::SPHERE: return 0;
            case ColliderType::AABB: return 1;
            case ColliderType::OBB: return 2;
            case ColliderType::CAPSULE: return 3;
            case ColliderType::CYLINDER: return 4;
            case ColliderType::CONVEX_HULL: return 5;
            case ColliderType::TRIANGLE_MESH: return 6;
            case ColliderType::HEIGHT_FIELD: return 7;
            }
            return 8;
        }

        void AppendContact(const CollisionPair& pair, ContactPoint contact,
                           std::vector<ContactPoint>& outContacts)
        {
            contact.bodyA = pair.colliderA->body;
            contact.bodyB = pair.colliderB->body;
            contact.colliderA = pair.colliderA->collider;
            contact.colliderB = pair.colliderB->collider;
            contact.materialA = pair.colliderA->material;
            contact.materialB = pair.colliderB->material;
            contact.isTrigger = pair.colliderA->isTrigger || pair.colliderB->isTrigger;
            if (contact.bodyA && contact.bodyB)
            {
                /// @note 形状ごとの法線を bodyB → bodyA へ揃える。
                const math::Vector3 bodyDelta = contact.bodyA->GetPosition() - contact.bodyB->GetPosition();
                if (bodyDelta.LengthSq() > 1e-8f &&
                    math::Vector3::Dot(contact.normal, bodyDelta) < 0.0f)
                {
                    contact.normal = -contact.normal;
                }
            }
            outContacts.push_back(contact);
        }
    }

    void PhysicsSolver::NarrowPhase(const std::vector<CollisionPair>& pairs,
                                    std::vector<ContactPoint>& outContacts)
    {
        const auto testOrderedPair = [this](const Collider& shapeA, ColliderType typeA,
                                           const Collider& shapeB, ColliderType typeB,
                                           ContactPoint& outContact) -> bool
        {
            switch (typeB)
            {
            case ColliderType::SPHERE:
                return typeA == ColliderType::SPHERE &&
                    TestSphereSphere(static_cast<const SphereCollider&>(shapeA),
                                     static_cast<const SphereCollider&>(shapeB), outContact);
            case ColliderType::AABB:
            {
                const auto& second = static_cast<const AABBCollider&>(shapeB);
                switch (typeA)
                {
                case ColliderType::SPHERE:
                    return TestSphereAABB(static_cast<const SphereCollider&>(shapeA), second, outContact);
                case ColliderType::AABB:
                    return TestAABBAABB(static_cast<const AABBCollider&>(shapeA), second, outContact);
                default:
                    return false;
                }
            }
            case ColliderType::OBB:
            {
                const auto& second = static_cast<const OBBCollider&>(shapeB);
                switch (typeA)
                {
                case ColliderType::SPHERE:
                    return TestSphereOBB(static_cast<const SphereCollider&>(shapeA), second, outContact);
                case ColliderType::AABB:
                    return TestAABBOBB(static_cast<const AABBCollider&>(shapeA), second, outContact);
                case ColliderType::OBB:
                    return TestOBBOBB(static_cast<const OBBCollider&>(shapeA), second, outContact);
                default:
                    return false;
                }
            }
            case ColliderType::CAPSULE:
            {
                const auto& second = static_cast<const CapsuleCollider&>(shapeB);
                switch (typeA)
                {
                case ColliderType::SPHERE:
                    return TestSphereCapsule(static_cast<const SphereCollider&>(shapeA), second, outContact);
                case ColliderType::AABB:
                    return TestAABBCapsule(static_cast<const AABBCollider&>(shapeA), second, outContact);
                case ColliderType::OBB:
                    return TestOBBCapsule(static_cast<const OBBCollider&>(shapeA), second, outContact);
                case ColliderType::CAPSULE:
                    return TestCapsuleCapsule(static_cast<const CapsuleCollider&>(shapeA), second, outContact);
                default:
                    return false;
                }
            }
            case ColliderType::CYLINDER:
            {
                const auto& second = static_cast<const CylinderCollider&>(shapeB);
                switch (typeA)
                {
                case ColliderType::SPHERE:
                    return TestSphereCylinder(static_cast<const SphereCollider&>(shapeA), second, outContact);
                case ColliderType::AABB:
                    return TestAABBCylinder(static_cast<const AABBCollider&>(shapeA), second, outContact);
                case ColliderType::OBB:
                    return TestOBBCylinder(static_cast<const OBBCollider&>(shapeA), second, outContact);
                case ColliderType::CAPSULE:
                    return TestCapsuleCylinder(static_cast<const CapsuleCollider&>(shapeA), second, outContact);
                case ColliderType::CYLINDER:
                    return TestCylinderCylinder(static_cast<const CylinderCollider&>(shapeA), second, outContact);
                default:
                    return false;
                }
            }
            case ColliderType::CONVEX_HULL:
            {
                const auto& second = static_cast<const ConvexHullCollider&>(shapeB);
                switch (typeA)
                {
                case ColliderType::SPHERE:
                    return TestSphereConvex(static_cast<const SphereCollider&>(shapeA), second, outContact);
                case ColliderType::AABB:
                    return TestAABBConvex(static_cast<const AABBCollider&>(shapeA), second, outContact);
                case ColliderType::OBB:
                    return TestOBBConvex(static_cast<const OBBCollider&>(shapeA), second, outContact);
                case ColliderType::CAPSULE:
                    return TestCapsuleConvex(static_cast<const CapsuleCollider&>(shapeA), second, outContact);
                case ColliderType::CYLINDER:
                    return TestCylinderConvex(static_cast<const CylinderCollider&>(shapeA), second, outContact);
                case ColliderType::CONVEX_HULL:
                    return TestConvexConvex(static_cast<const ConvexHullCollider&>(shapeA), second, outContact);
                default:
                    return false;
                }
            }
            case ColliderType::TRIANGLE_MESH:
            {
                const auto& second = static_cast<const TriangleMeshCollider&>(shapeB);
                switch (typeA)
                {
                case ColliderType::SPHERE:
                    return TestSphereTriangleMesh(static_cast<const SphereCollider&>(shapeA), second, outContact);
                case ColliderType::AABB:
                    return TestAABBTriangleMesh(static_cast<const AABBCollider&>(shapeA), second, outContact);
                case ColliderType::OBB:
                    return TestOBBTriangleMesh(static_cast<const OBBCollider&>(shapeA), second, outContact);
                case ColliderType::CAPSULE:
                    return TestCapsuleTriangleMesh(static_cast<const CapsuleCollider&>(shapeA), second, outContact);
                case ColliderType::CYLINDER:
                    return TestCylinderTriangleMesh(static_cast<const CylinderCollider&>(shapeA), second, outContact);
                case ColliderType::CONVEX_HULL:
                    return TestConvexHullTriangleMesh(static_cast<const ConvexHullCollider&>(shapeA), second, outContact);
                default:
                    return false;
                }
            }
            case ColliderType::HEIGHT_FIELD:
            {
                const auto& second = static_cast<const HeightFieldCollider&>(shapeB);
                switch (typeA)
                {
                case ColliderType::SPHERE:
                    return TestSphereHeightField(static_cast<const SphereCollider&>(shapeA), second, outContact);
                case ColliderType::AABB:
                    return TestAABBHeightField(static_cast<const AABBCollider&>(shapeA), second, outContact);
                case ColliderType::OBB:
                    return TestOBBHeightField(static_cast<const OBBCollider&>(shapeA), second, outContact);
                case ColliderType::CAPSULE:
                    return TestCapsuleHeightField(static_cast<const CapsuleCollider&>(shapeA), second, outContact);
                case ColliderType::CYLINDER:
                    return TestCylinderHeightField(static_cast<const CylinderCollider&>(shapeA), second, outContact);
                case ColliderType::CONVEX_HULL:
                    return TestConvexHullHeightField(static_cast<const ConvexHullCollider&>(shapeA), second, outContact);
                default:
                    return false;
                }
            }
            }
            return false;
        };

        for (const CollisionPair& pair : pairs)
        {
            const Collider& shapeA = *pair.colliderA->collider;
            const Collider& shapeB = *pair.colliderB->collider;
            const ColliderType typeA = shapeA.GetType();
            const ColliderType typeB = shapeB.GetType();
            const bool swapped = ContactShapeOrder(typeA) > ContactShapeOrder(typeB);

            ContactPoint baseContact;
            const bool hit = swapped
                ? testOrderedPair(shapeB, typeB, shapeA, typeA, baseContact)
                : testOrderedPair(shapeA, typeA, shapeB, typeB, baseContact);
            if (!hit) continue;
            if (swapped) baseContact.normal = -baseContact.normal;

            const auto appendManifold = [&](const ContactManifold& manifold)
            {
                for (int pointIndex = 0; pointIndex < manifold.count; ++pointIndex)
                    AppendContact(pair, manifold.points[pointIndex], outContacts);
            };
            if (typeA == ColliderType::AABB && typeB == ColliderType::AABB)
                appendManifold(BuildAABBManifold(shapeA.GetAABB(), shapeB.GetAABB(), baseContact));
            else if (typeA == ColliderType::OBB && typeB == ColliderType::OBB)
                appendManifold(BuildOBBManifold(static_cast<const OBBCollider&>(shapeA),
                                                static_cast<const OBBCollider&>(shapeB), baseContact));
            else
                AppendContact(pair, baseContact, outContacts);
        }
    }
}
