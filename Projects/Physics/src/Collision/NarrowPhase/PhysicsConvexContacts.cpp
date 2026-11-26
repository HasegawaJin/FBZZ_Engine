/// @file    PhysicsConvexContacts.cpp
/// @brief   サポート関数を使った GJK・EPA の接触判定。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/Collision/NarrowPhase/PhysicsSolver.hpp>
#include "PhysicsContactGeometry.hpp"
#include <Physics/Collision/NarrowPhase/EPA.hpp>
#include <Physics/Collision/NarrowPhase/GJK.hpp>

namespace fbzz::physics
{
    namespace
    {
        /// @brief GJK が渡す探索方向は «長さ 0 になりうる» (同心球・入れ子カプセル・退化した単体)。
        /// @note サポート関数は全域で定義される必要があり (部分関数だと単体が組めない)、
        /// @note 方向が無いときに返す点は結果に影響しないため固定方向で振動を防ぐ。
        /// @note Normalized() は長さ 0 で assert する契約だが、ここは退化が正常系のため
        /// @note NormalizedOr を使う。
        math::Vector3 SupportDirection(const math::Vector3& dir)
        {
            return dir.NormalizedOr(math::Vector3::UP);
        }

        /// @note 各形状の GJK サポート関数
        math::Vector3 SupportSphere(const void* shape, const math::Vector3& dir)
        {
            const auto* s = static_cast<const SphereCollider*>(shape);
            const math::Vector3 center = s->GetAABB().Center();
            return center + SupportDirection(dir) * s->m_radius;
        }

        math::Vector3 SupportAABB(const void* shape, const math::Vector3& dir)
        {
            const AABB aabb = static_cast<const AABBCollider*>(shape)->GetAABB();
            return {
                dir.x >= 0.0f ? aabb.max.x : aabb.min.x,
                dir.y >= 0.0f ? aabb.max.y : aabb.min.y,
                dir.z >= 0.0f ? aabb.max.z : aabb.min.z
            };
        }

        math::Vector3 SupportOBB(const void* shape, const math::Vector3& dir)
        {
            return static_cast<const OBBCollider*>(shape)->SupportPoint(dir);
        }

        math::Vector3 SupportCapsule(const void* shape, const math::Vector3& dir)
        {
            const auto* c = static_cast<const CapsuleCollider*>(shape);
            const math::Vector3 s = c->GetSegmentStart();
            const math::Vector3 e = c->GetSegmentEnd();
            // セグメント上で dir と最も内積が大きい点 + radius
            const math::Vector3 best = (math::Vector3::Dot(s, dir) >= math::Vector3::Dot(e, dir))
                                        ? s : e;
            return best + SupportDirection(dir) * c->m_radius;
        }

        math::Vector3 SupportTriangle(const void* shape, const math::Vector3& direction)
        {
            const auto* triangle = static_cast<const Triangle*>(shape);
            const float projection0 = math::Vector3::Dot(triangle->v[0], direction);
            const float projection1 = math::Vector3::Dot(triangle->v[1], direction);
            const float projection2 = math::Vector3::Dot(triangle->v[2], direction);
            if (projection0 >= projection1 && projection0 >= projection2) {
                return triangle->v[0];
            }
            return projection1 >= projection2 ? triangle->v[1] : triangle->v[2];
        }

        /// @note GJK (Simplex 付き) + EPA から ContactPoint を構築するヘルパー
        bool GJKEPAToContact(const void* shapeA, SupportFn supportA,
                             const void* shapeB, SupportFn supportB,
                             ContactPoint& out)
        {
            const GJKResult gjk = GJK_Intersect(shapeA, supportA, shapeB, supportB);
            if (!gjk.intersects) {
                return false;
            }

            const EPAResult epa = EPA_GetContactInfo(shapeA, supportA, shapeB, supportB, gjk.simplex);
            if (!epa.valid) {
                return false;
            }

            out.normal = epa.normal;
            out.depth  = epa.depth;
            out.point  = (epa.contactA + epa.contactB) * 0.5f;
            return true;
        }
    }

    bool contact_geometry_detail::TestConvexTriangle(const void* shape, SupportFn supportShape,
                                                       const Triangle& triangle, ContactPoint& outContact)
    {
        return GJKEPAToContact(shape, supportShape, &triangle, SupportTriangle, outContact);
    }

    bool PhysicsSolver::TestAABBOBB(const AABBCollider& a,
                                    const OBBCollider& b,
                                    ContactPoint& out)
    {
        return GJKEPAToContact(&a, SupportAABB, &b, SupportOBB, out);
    }

    bool PhysicsSolver::TestConvexConvex(const ConvexHullCollider& a,
                                          const ConvexHullCollider& b,
                                          ContactPoint& out)
    {
        return GJKEPAToContact(&a, ConvexHullCollider::SupportFnImpl,
                               &b, ConvexHullCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestSphereConvex(const SphereCollider& s,
                                          const ConvexHullCollider& hull,
                                          ContactPoint& out)
    {
        return GJKEPAToContact(&s,    SupportSphere,
                               &hull, ConvexHullCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestAABBConvex(const AABBCollider& b,
                                        const ConvexHullCollider& hull,
                                        ContactPoint& out)
    {
        return GJKEPAToContact(&b,    SupportAABB,
                               &hull, ConvexHullCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestOBBConvex(const OBBCollider& b,
                                      const ConvexHullCollider& hull,
                                      ContactPoint& out)
    {
        return GJKEPAToContact(&b,    SupportOBB,
                               &hull, ConvexHullCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestCapsuleConvex(const CapsuleCollider& c,
                                           const ConvexHullCollider& hull,
                                           ContactPoint& out)
    {
        return GJKEPAToContact(&c,    SupportCapsule,
                               &hull, ConvexHullCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestAABBCylinder(const AABBCollider& b,
                                          const CylinderCollider& c,
                                          ContactPoint& out)
    {
        return GJKEPAToContact(&b, SupportAABB,
                               &c, CylinderCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestOBBCylinder(const OBBCollider& b,
                                         const CylinderCollider& c,
                                         ContactPoint& out)
    {
        return GJKEPAToContact(&b, SupportOBB,
                               &c, CylinderCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestCapsuleCylinder(const CapsuleCollider& a,
                                             const CylinderCollider& c,
                                             ContactPoint& out)
    {
        return GJKEPAToContact(&a, SupportCapsule,
                               &c, CylinderCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestCylinderCylinder(const CylinderCollider& a,
                                              const CylinderCollider& b,
                                              ContactPoint& out)
    {
        return GJKEPAToContact(&a, CylinderCollider::SupportFnImpl,
                               &b, CylinderCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestCylinderConvex(const CylinderCollider& c,
                                            const ConvexHullCollider& hull,
                                            ContactPoint& out)
    {
        return GJKEPAToContact(&c,    CylinderCollider::SupportFnImpl,
                               &hull, ConvexHullCollider::SupportFnImpl, out);
    }
}
