// FBZZ Engine
// PhysicsSolver.cpp | fbzz::physics
// 衝突検出 (Broad/Narrow フェーズ) と衝突解決 (インパルスベース)
#include <Physics/PhysicsSolver.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include <Physics/GJK.hpp>
#include <Physics/EPA.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace fbzz::physics 
{
    namespace
    {
        void ClosestPointsOnSegments(const math::Vector3& p1,
                                     const math::Vector3& q1,
                                     const math::Vector3& p2,
                                     const math::Vector3& q2,
                                     math::Vector3& outC1,
                                     math::Vector3& outC2)
        {
            constexpr float EPS = 1e-6f;

            const math::Vector3 d1 = q1 - p1;
            const math::Vector3 d2 = q2 - p2;
            const math::Vector3 r = p1 - p2;
            const float a = math::Vector3::Dot(d1, d1);
            const float e = math::Vector3::Dot(d2, d2);
            const float f = math::Vector3::Dot(d2, r);

            float s = 0.0f;
            float t = 0.0f;

            if (a <= EPS && e <= EPS)
            {
                outC1 = p1;
                outC2 = p2;
                return;
            }

            if (a <= EPS)
            {
                t = std::clamp(f / e, 0.0f, 1.0f);
            }
            else
            {
                const float c = math::Vector3::Dot(d1, r);
                if (e <= EPS)
                {
                    s = std::clamp(-c / a, 0.0f, 1.0f);
                }
                else
                {
                    const float b = math::Vector3::Dot(d1, d2);
                    const float denom = a * e - b * b;
                    if (denom > EPS)
                    {
                        s = std::clamp((b * f - c * e) / denom, 0.0f, 1.0f);
                    }
                    else
                    {
                        const float s0 = math::Vector3::Dot(p2 - p1, d1) / a;
                        const float s1 = math::Vector3::Dot(q2 - p1, d1) / a;
                        const float overlapMin = std::max(0.0f, std::min(s0, s1));
                        const float overlapMax = std::min(1.0f, std::max(s0, s1));
                        s = overlapMin <= overlapMax
                            ? (overlapMin + overlapMax) * 0.5f
                            : std::clamp((s0 + s1) * 0.5f, 0.0f, 1.0f);
                    }

                    t = (b * s + f) / e;
                    if (t < 0.0f)
                    {
                        t = 0.0f;
                        s = std::clamp(-c / a, 0.0f, 1.0f);
                    }
                    else if (t > 1.0f)
                    {
                        t = 1.0f;
                        s = std::clamp((b - c) / a, 0.0f, 1.0f);
                    }
                }
            }

            outC1 = p1 + d1 * s;
            outC2 = p2 + d2 * t;
        }
    } // namespace

    // ------------------------------------------------------------------ BroadPhase
    void PhysicsSolver::BroadPhase(const std::vector<ColliderInstance>& colliders,
                                    std::vector<CollisionPair>& outPairs,
                                    const std::function<bool(int, int)>& layerFilter)
    {
        for (size_t i = 0; i < colliders.size(); ++i)
        {
            if (!colliders[i].collider) continue;
            for (size_t j = i + 1; j < colliders.size(); ++j)
            {
                if (!colliders[j].collider) continue;
                if (layerFilter && !layerFilter(colliders[i].layer, colliders[j].layer)) continue;
                if (!colliders[i].isTrigger && !colliders[j].isTrigger)
                {
                    const bool staticA = !colliders[i].body || colliders[i].body->IsStatic();
                    const bool staticB = !colliders[j].body || colliders[j].body->IsStatic();
                    if (staticA && staticB) continue;
                }

                if (colliders[i].collider->GetAABB().Overlaps(colliders[j].collider->GetAABB()))
                {
                    outPairs.push_back({ colliders[i], colliders[j] });
                }
            }
        }
    }

    // ----------------------------------------------------------------- NarrowPhase
    void PhysicsSolver::NarrowPhase(const std::vector<CollisionPair>& pairs,
                                    std::vector<ContactPoint>& outContacts)
    {
        for (auto& pair : pairs)
        {
            ColliderType tA = pair.colliderA.collider->GetType();
            ColliderType tB = pair.colliderB.collider->GetType();
            ContactPoint cp;
            bool hit = false;

            if (tA == ColliderType::SPHERE && tB == ColliderType::SPHERE)
            {
                hit = TestSphereSphere(
                    *static_cast<SphereCollider*>(pair.colliderA.collider.get()),
                    *static_cast<SphereCollider*>(pair.colliderB.collider.get()), cp);
            }
            else if (tA == ColliderType::AABB && tB == ColliderType::AABB)
            {
                hit = TestAABBAABB(
                    *static_cast<AABBCollider*>(pair.colliderA.collider.get()),
                    *static_cast<AABBCollider*>(pair.colliderB.collider.get()), cp);
            }
            else if (tA == ColliderType::SPHERE && tB == ColliderType::AABB)
            {
                hit = TestSphereAABB(
                    *static_cast<SphereCollider*>(pair.colliderA.collider.get()),
                    *static_cast<AABBCollider*> (pair.colliderB.collider.get()), cp);
            }
            else if (tA == ColliderType::AABB && tB == ColliderType::SPHERE)
            {
                // 引数順を正規化して呼び、法線を反転する
                hit = TestSphereAABB(
                    *static_cast<SphereCollider*>(pair.colliderB.collider.get()),
                    *static_cast<AABBCollider*> (pair.colliderA.collider.get()), cp);
                if (hit)
                {
                    cp.normal  = -cp.normal;
                }
            }
            else if (tA == ColliderType::SPHERE && tB == ColliderType::CAPSULE)
            {
                hit = TestSphereCapsule(
                    *static_cast<SphereCollider*>(pair.colliderA.collider.get()),
                    *static_cast<CapsuleCollider*>(pair.colliderB.collider.get()), cp);
            }
            else if (tA == ColliderType::CAPSULE && tB == ColliderType::SPHERE)
            {
                hit = TestSphereCapsule(
                    *static_cast<SphereCollider*>(pair.colliderB.collider.get()),
                    *static_cast<CapsuleCollider*>(pair.colliderA.collider.get()), cp);
                if (hit)
                {
                    cp.normal = -cp.normal;
                }
            }
            else if (tA == ColliderType::AABB && tB == ColliderType::CAPSULE)
            {
                hit = TestAABBCapsule(
                    *static_cast<AABBCollider*>(pair.colliderA.collider.get()),
                    *static_cast<CapsuleCollider*>(pair.colliderB.collider.get()), cp);
            }
            else if (tA == ColliderType::CAPSULE && tB == ColliderType::AABB)
            {
                hit = TestAABBCapsule(
                    *static_cast<AABBCollider*>(pair.colliderB.collider.get()),
                    *static_cast<CapsuleCollider*>(pair.colliderA.collider.get()), cp);
                if (hit)
                {
                    cp.normal = -cp.normal;
                }
            }
            else if (tA == ColliderType::CAPSULE && tB == ColliderType::CAPSULE)
            {
                hit = TestCapsuleCapsule(
                    *static_cast<CapsuleCollider*>(pair.colliderA.collider.get()),
                    *static_cast<CapsuleCollider*>(pair.colliderB.collider.get()), cp);
            }

            else if (tA == ColliderType::CONVEX_HULL || tB == ColliderType::CONVEX_HULL)
            {
                // CONVEX_HULL を含むペアは GJK + EPA で処理する
                // CONVEX_HULL vs CONVEX_HULL
                if (tA == ColliderType::CONVEX_HULL && tB == ColliderType::CONVEX_HULL)
                {
                    hit = TestConvexConvex(
                        *static_cast<ConvexHullCollider*>(pair.colliderA.collider.get()),
                        *static_cast<ConvexHullCollider*>(pair.colliderB.collider.get()), cp);
                }
                else
                {
                    // ConvexHull を常に B 側に正規化する
                    const bool swapped = (tA == ColliderType::CONVEX_HULL);
                    const ColliderInstance& dynInst  = swapped ? pair.colliderB : pair.colliderA;
                    const ColliderInstance& convInst = swapped ? pair.colliderA : pair.colliderB;
                    const ColliderType dynType = dynInst.collider->GetType();
                    const auto& hull = *static_cast<ConvexHullCollider*>(convInst.collider.get());

                    if (dynType == ColliderType::SPHERE)
                        hit = TestSphereConvex(
                            *static_cast<SphereCollider*>(dynInst.collider.get()), hull, cp);
                    else if (dynType == ColliderType::AABB)
                        hit = TestAABBConvex(
                            *static_cast<AABBCollider*>(dynInst.collider.get()), hull, cp);
                    else if (dynType == ColliderType::CAPSULE)
                        hit = TestCapsuleConvex(
                            *static_cast<CapsuleCollider*>(dynInst.collider.get()), hull, cp);

                    if (hit && swapped)
                        cp.normal = -cp.normal;
                }
            }
            else if (tA == ColliderType::TRIANGLE_MESH || tB == ColliderType::TRIANGLE_MESH)
            {
                // TRIANGLE_MESH vs TRIANGLE_MESH は両方 Static なのでスキップ
                if (tA == ColliderType::TRIANGLE_MESH && tB == ColliderType::TRIANGLE_MESH)
                {
                    // skip
                }
                else
                {
                    // TRIANGLE_MESH を常に B 側に正規化する
                    const bool swapped = (tA == ColliderType::TRIANGLE_MESH);
                    const ColliderInstance& dynInst  = swapped ? pair.colliderB : pair.colliderA;
                    const ColliderInstance& meshInst = swapped ? pair.colliderA : pair.colliderB;
                    const ColliderType dynType = dynInst.collider->GetType();
                    const auto& mesh = *static_cast<TriangleMeshCollider*>(meshInst.collider.get());

                    if (dynType == ColliderType::SPHERE)
                        hit = TestSphereTriangleMesh(
                            *static_cast<SphereCollider*>(dynInst.collider.get()), mesh, cp);
                    else if (dynType == ColliderType::AABB)
                        hit = TestAABBTriangleMesh(
                            *static_cast<AABBCollider*>(dynInst.collider.get()), mesh, cp);
                    else if (dynType == ColliderType::CAPSULE)
                        hit = TestCapsuleTriangleMesh(
                            *static_cast<CapsuleCollider*>(dynInst.collider.get()), mesh, cp);

                    // スワップした場合は法線を反転 (normal は dyn → mesh 方向)
                    if (hit && swapped)
                        cp.normal = -cp.normal;
                }
            }

            if (hit) {
                cp.bodyA = pair.colliderA.body;
                cp.bodyB = pair.colliderB.body;
                cp.colliderA = pair.colliderA.collider.get();
                cp.colliderB = pair.colliderB.collider.get();
                cp.materialA = pair.colliderA.material;
                cp.materialB = pair.colliderB.material;
                cp.isTrigger = pair.colliderA.isTrigger || pair.colliderB.isTrigger;
                if (cp.bodyA && cp.bodyB)
                {
                    const math::Vector3 bodyDelta = cp.bodyA->GetPosition() - cp.bodyB->GetPosition();
                    if (bodyDelta.LengthSq() > 1e-8f &&
                        math::Vector3::Dot(cp.normal, bodyDelta) < 0.0f)
                    {
                        cp.normal = -cp.normal;
                    }
                }
                outContacts.push_back(cp);
            }
        }
    }

    // --------------------------------------------------------------------- Resolve
    void PhysicsSolver::Resolve(std::vector<ContactPoint>& contacts)
    {
        // 事前計算: 全接触点の摩擦タンジェント軸を確定する
        for (auto& cp : contacts)
        {
            if (cp.isTrigger) continue;

            // normal に対して垂直な 2 軸を Gram-Schmidt で構築
            math::Vector3 t0 = math::Vector3::Cross(cp.normal, math::Vector3::RIGHT);
            if (t0.LengthSq() < 1e-6f)
                t0 = math::Vector3::Cross(cp.normal, math::Vector3::UP);
            t0           = t0.Normalized();
            cp.tangent[0] = t0;
            cp.tangent[1] = math::Vector3::Cross(cp.normal, t0).Normalized();
        }

        for (int i = 0; i < VELOCITY_ITER; ++i)
        {
            for (auto& cp : contacts)
            {
                ResolveVelocity(cp);
                ResolveFriction(cp);
            }
        }

        for (auto& cp : contacts)
            ResolvePosition(cp);
    }

    // ---------------------------------------------------------- ResolveVelocity (PGS)
    void PhysicsSolver::ResolveVelocity(ContactPoint& cp)
    {
        if (cp.isTrigger) return;

        RigidBody* bodyA = cp.bodyA;
        RigidBody* bodyB = cp.bodyB;

        const float invMassA = bodyA ? bodyA->GetInvMass() : 0.0f;
        const float invMassB = bodyB ? bodyB->GetInvMass() : 0.0f;

        const math::Vector3 vA = bodyA ? bodyA->GetVelocity()        : math::Vector3::ZERO;
        const math::Vector3 vB = bodyB ? bodyB->GetVelocity()        : math::Vector3::ZERO;
        const math::Vector3 wA = bodyA ? bodyA->GetAngularVelocity() : math::Vector3::ZERO;
        const math::Vector3 wB = bodyB ? bodyB->GetAngularVelocity() : math::Vector3::ZERO;

        const math::Vector3 rA = bodyA ? cp.point - bodyA->GetPosition() : math::Vector3::ZERO;
        const math::Vector3 rB = bodyB ? cp.point - bodyB->GetPosition() : math::Vector3::ZERO;

        const math::Vector3 vAContact = vA + math::Vector3::Cross(wA, rA);
        const math::Vector3 vBContact = vB + math::Vector3::Cross(wB, rB);
        const math::Vector3 vRel      = vAContact - vBContact;
        const float         vRelN     = math::Vector3::Dot(vRel, cp.normal);

        if (vRelN > 0.0f) return;

        float e = 0.3f;
        if (cp.materialA && cp.materialB)
            e = PhysicsMaterial::CombineRestitution(*cp.materialA, *cp.materialB);

        constexpr float REST_THRESHOLD = 0.5f;
        if (std::abs(vRelN) < REST_THRESHOLD) e = 0.0f;
        const bool usesRestitution = e > 0.0f;

        float angTermA = 0.0f;
        float angTermB = 0.0f;
        if (bodyA)
        {
            const math::Vector3 rAxN = math::Vector3::Cross(rA, cp.normal);
            angTermA = math::Vector3::Dot(
                math::Vector3::Cross(bodyA->ApplyInvInertia(rAxN), rA), cp.normal);
        }
        if (bodyB)
        {
            const math::Vector3 rBxN = math::Vector3::Cross(rB, cp.normal);
            angTermB = math::Vector3::Dot(
                math::Vector3::Cross(bodyB->ApplyInvInertia(rBxN), rB), cp.normal);
        }

        const float denom = invMassA + invMassB + angTermA + angTermB;
        if (denom == 0.0f) return;

        const float deltaJ = -(1.0f + e) * vRelN / denom;
        float applyJ = 0.0f;
        if (usesRestitution)
        {
            applyJ = std::max(0.0f, deltaJ);
            cp.cachedNormalImpulse = applyJ;
            cp.cacheImpulse = false;
        }
        else
        {
            const float oldAccum = cp.cachedNormalImpulse;
            const float newAccum = std::max(0.0f, oldAccum + deltaJ);
            applyJ = newAccum - oldAccum;
            cp.cachedNormalImpulse = newAccum;
        }

        const math::Vector3 impulse = cp.normal * applyJ;
        if (bodyA)
        {
            bodyA->SetVelocity(bodyA->GetVelocity() + impulse * invMassA);
            bodyA->ApplyAngularImpulse(math::Vector3::Cross(rA, impulse));
        }
        if (bodyB)
        {
            bodyB->SetVelocity(bodyB->GetVelocity() - impulse * invMassB);
            bodyB->ApplyAngularImpulse(-math::Vector3::Cross(rB, impulse));
        }
    }

    // ---------------------------------------------------------- ResolveFriction (PGS)
    void PhysicsSolver::ResolveFriction(ContactPoint& cp)
    {
        if (cp.isTrigger) return;

        RigidBody* bodyA = cp.bodyA;
        RigidBody* bodyB = cp.bodyB;

        const float invMassA = bodyA ? bodyA->GetInvMass() : 0.0f;
        const float invMassB = bodyB ? bodyB->GetInvMass() : 0.0f;
        if (invMassA + invMassB == 0.0f) return;

        float mu = 0.5f;
        if (cp.materialA && cp.materialB)
            mu = PhysicsMaterial::CombineFriction(*cp.materialA, *cp.materialB);

        // 摩擦コーン制約: |Λt| ≤ μ * Λn
        const float maxFriction = mu * cp.cachedNormalImpulse;

        const math::Vector3 rA = bodyA ? cp.point - bodyA->GetPosition() : math::Vector3::ZERO;
        const math::Vector3 rB = bodyB ? cp.point - bodyB->GetPosition() : math::Vector3::ZERO;

        const math::Vector3 vA = bodyA ? bodyA->GetVelocity()        : math::Vector3::ZERO;
        const math::Vector3 vB = bodyB ? bodyB->GetVelocity()        : math::Vector3::ZERO;
        const math::Vector3 wA = bodyA ? bodyA->GetAngularVelocity() : math::Vector3::ZERO;
        const math::Vector3 wB = bodyB ? bodyB->GetAngularVelocity() : math::Vector3::ZERO;

        const math::Vector3 vRel = (vA + math::Vector3::Cross(wA, rA))
                                 - (vB + math::Vector3::Cross(wB, rB));

        for (int k = 0; k < 2; ++k)
        {
            const math::Vector3& t     = cp.tangent[k];
            const float          vRelT = math::Vector3::Dot(vRel, t);

            float angA = 0.0f;
            float angB = 0.0f;
            if (bodyA)
            {
                const math::Vector3 rAxT = math::Vector3::Cross(rA, t);
                angA = math::Vector3::Dot(math::Vector3::Cross(bodyA->ApplyInvInertia(rAxT), rA), t);
            }
            if (bodyB)
            {
                const math::Vector3 rBxT = math::Vector3::Cross(rB, t);
                angB = math::Vector3::Dot(math::Vector3::Cross(bodyB->ApplyInvInertia(rBxT), rB), t);
            }

            const float denom = invMassA + invMassB + angA + angB;
            if (denom == 0.0f) continue;

            const float deltaJt  = -vRelT / denom;
            const float oldAccum = cp.cachedTangentImpulse[k];
            const float newAccum = std::clamp(oldAccum + deltaJt, -maxFriction, maxFriction);
            const float applyJt  = newAccum - oldAccum;
            cp.cachedTangentImpulse[k] = newAccum;

            const math::Vector3 impulse = t * applyJt;
            if (bodyA)
            {
                bodyA->SetVelocity(bodyA->GetVelocity() + impulse * invMassA);
                bodyA->ApplyAngularImpulse(math::Vector3::Cross(rA, impulse));
            }
            if (bodyB)
            {
                bodyB->SetVelocity(bodyB->GetVelocity() - impulse * invMassB);
                bodyB->ApplyAngularImpulse(-math::Vector3::Cross(rB, impulse));
            }
        }
    }

    // ---------------------------------------------------------- ResolvePosition
    void PhysicsSolver::ResolvePosition(ContactPoint& cp)
    {
        if (cp.isTrigger) return;

        const float invMassA = cp.bodyA ? cp.bodyA->GetInvMass() : 0.0f;
        const float invMassB = cp.bodyB ? cp.bodyB->GetInvMass() : 0.0f;
        const float invMassSum = invMassA + invMassB;
        if (invMassSum == 0.0f) return;

        const float penetration = std::max(cp.depth - SLOP, 0.0f);
        const float scalar      = penetration / invMassSum * BAUMGARTE;
        math::Vector3 correction = cp.normal * scalar;

        if (cp.bodyA)
            cp.bodyA->SetPosition(cp.bodyA->GetPosition() + correction * invMassA);
        if (cp.bodyB)
            cp.bodyB->SetPosition(cp.bodyB->GetPosition() - correction * invMassB);
    }

    // ------------------------------------------------------- Narrow phase テスト関数

    bool PhysicsSolver::TestSphereSphere(const SphereCollider& a, const SphereCollider& b,
                                        ContactPoint& out)
    {
        math::Vector3 posA = a.GetAABB().Center();
        math::Vector3 posB = b.GetAABB().Center();
        math::Vector3 diff = posA - posB;
        float         dist = diff.Length();
        float         sumR = a.m_radius + b.m_radius;

        if (dist >= sumR || dist < 1e-6f) return false;

        out.normal = diff * (1.0f / dist);
        out.depth  = sumR - dist;
        out.point  = posB + out.normal * b.m_radius;
        return true;
    }

    bool PhysicsSolver::TestAABBAABB(const AABBCollider& a, const AABBCollider& b,
                                    ContactPoint& out)
    {
        AABB aabbA = a.GetAABB();
        AABB aabbB = b.GetAABB();

        float ox = std::min(aabbA.max.x, aabbB.max.x) - std::max(aabbA.min.x, aabbB.min.x);
        float oy = std::min(aabbA.max.y, aabbB.max.y) - std::max(aabbA.min.y, aabbB.min.y);
        float oz = std::min(aabbA.max.z, aabbB.max.z) - std::max(aabbA.min.z, aabbB.min.z);

        if (ox <= 0.0f || oy <= 0.0f || oz <= 0.0f) return false;

        math::Vector3 dir = aabbA.Center() - aabbB.Center();

        if (ox <= oy && ox <= oz)
        {
            out.depth  = ox;
            out.normal = { dir.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f };
            out.point = {
                out.normal.x > 0.0f ? (aabbA.min.x + aabbB.max.x) * 0.5f
                                    : (aabbA.max.x + aabbB.min.x) * 0.5f,
                (std::max(aabbA.min.y, aabbB.min.y) + std::min(aabbA.max.y, aabbB.max.y)) * 0.5f,
                (std::max(aabbA.min.z, aabbB.min.z) + std::min(aabbA.max.z, aabbB.max.z)) * 0.5f
            };
        }
        else if (oy <= ox && oy <= oz)
        {
            out.depth  = oy;
            out.normal = { 0.0f, dir.y >= 0.0f ? 1.0f : -1.0f, 0.0f };
            out.point = {
                (std::max(aabbA.min.x, aabbB.min.x) + std::min(aabbA.max.x, aabbB.max.x)) * 0.5f,
                out.normal.y > 0.0f ? (aabbA.min.y + aabbB.max.y) * 0.5f
                                    : (aabbA.max.y + aabbB.min.y) * 0.5f,
                (std::max(aabbA.min.z, aabbB.min.z) + std::min(aabbA.max.z, aabbB.max.z)) * 0.5f
            };
        }
        else
        {
            out.depth  = oz;
            out.normal = { 0.0f, 0.0f, dir.z >= 0.0f ? 1.0f : -1.0f };
            out.point = {
                (std::max(aabbA.min.x, aabbB.min.x) + std::min(aabbA.max.x, aabbB.max.x)) * 0.5f,
                (std::max(aabbA.min.y, aabbB.min.y) + std::min(aabbA.max.y, aabbB.max.y)) * 0.5f,
                out.normal.z > 0.0f ? (aabbA.min.z + aabbB.max.z) * 0.5f
                                    : (aabbA.max.z + aabbB.min.z) * 0.5f
            };
        }

        return true;
    }

    bool PhysicsSolver::TestSphereAABB(const SphereCollider& s, const AABBCollider& b,
                                        ContactPoint& out)
    {
        math::Vector3 center = s.GetAABB().Center();
        AABB          aabb   = b.GetAABB();

        math::Vector3 closest = {
            std::max(aabb.min.x, std::min(center.x, aabb.max.x)),
            std::max(aabb.min.y, std::min(center.y, aabb.max.y)),
            std::max(aabb.min.z, std::min(center.z, aabb.max.z))
        };

        math::Vector3 diff = center - closest;
        float         dist = diff.Length();

        if (dist >= s.m_radius) return false;

        if (dist < 1e-6f)
        {
            // 球中心がAABB内部: 最も浅い面に押し出す
            out.normal = math::Vector3::UP;
            out.depth  = s.m_radius;
        }
        else
        {
            out.normal = diff * (1.0f / dist);
            out.depth  = s.m_radius - dist;
        }

        out.point = closest;
        return true;
    }

    bool PhysicsSolver::TestSphereCapsule(const SphereCollider& s,
                                        const CapsuleCollider& c,
                                        ContactPoint& out)
    {
        const math::Vector3 center = s.GetAABB().Center();
        const math::Vector3 segStart = c.GetSegmentStart();
        const math::Vector3 segEnd = c.GetSegmentEnd();
        const math::Vector3 seg = segEnd - segStart;
        const float segLenSq = seg.LengthSq();
        float t = 0.0f;
        if (segLenSq > 1e-6f)
            t = std::clamp(math::Vector3::Dot(center - segStart, seg) / segLenSq, 0.0f, 1.0f);

        const math::Vector3 closest = segStart + seg * t;
        const math::Vector3 diff = center - closest;
        const float dist = diff.Length();
        const float sumR = s.m_radius + c.m_radius;
        if (dist >= sumR) return false;

        out.normal = dist < 1e-6f ? math::Vector3::UP : diff * (1.0f / dist);
        out.depth = sumR - dist;
        out.point = closest + out.normal * c.m_radius;
        return true;
    }

    bool PhysicsSolver::TestAABBCapsule(const AABBCollider& b,
                                        const CapsuleCollider& c,
                                        ContactPoint& out)
    {
        AABB aabb = b.GetAABB();
        math::Vector3 bestCapsulePoint = c.GetSegmentStart();
        math::Vector3 bestBoxPoint = aabb.Center();
        float bestDistSq = 3.402823466e+38f;

        for (int i = 0; i <= 6; ++i)
        {
            const float t = static_cast<float>(i) / 6.0f;
            const math::Vector3 p = c.GetSegmentStart() + (c.GetSegmentEnd() - c.GetSegmentStart()) * t;
            const math::Vector3 q = {
                std::max(aabb.min.x, std::min(p.x, aabb.max.x)),
                std::max(aabb.min.y, std::min(p.y, aabb.max.y)),
                std::max(aabb.min.z, std::min(p.z, aabb.max.z))
            };
            const float distSq = (p - q).LengthSq();
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                bestCapsulePoint = p;
                bestBoxPoint = q;
            }
        }

        if (bestDistSq >= c.m_radius * c.m_radius) return false;

        const float dist = std::sqrt(bestDistSq);
        if (dist < 1e-6f)
        {
            const math::Vector3 dir = bestCapsulePoint - aabb.Center();
            if (std::abs(dir.x) >= std::abs(dir.y) && std::abs(dir.x) >= std::abs(dir.z))
                out.normal = { dir.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f };
            else if (std::abs(dir.y) >= std::abs(dir.z))
                out.normal = { 0.0f, dir.y >= 0.0f ? 1.0f : -1.0f, 0.0f };
            else
                out.normal = { 0.0f, 0.0f, dir.z >= 0.0f ? 1.0f : -1.0f };
            out.depth = c.m_radius;
        }
        else
        {
            out.normal = (bestBoxPoint - bestCapsulePoint) * (1.0f / dist);
            out.depth = c.m_radius - dist;
        }

        out.point = bestBoxPoint;
        return true;
    }

    bool PhysicsSolver::TestCapsuleCapsule(const CapsuleCollider& a,
                                           const CapsuleCollider& b,
                                           ContactPoint& out)
    {
        math::Vector3 bestA;
        math::Vector3 bestB;
        ClosestPointsOnSegments(a.GetSegmentStart(), a.GetSegmentEnd(),
                                b.GetSegmentStart(), b.GetSegmentEnd(),
                                bestA, bestB);

        const float sumR = a.m_radius + b.m_radius;
        const float bestDistSq = (bestA - bestB).LengthSq();
        if (bestDistSq >= sumR * sumR) return false;

        const float dist = std::sqrt(bestDistSq);
        out.normal = dist < 1e-6f ? math::Vector3::UP : (bestA - bestB) * (1.0f / dist);
        out.depth = sumR - dist;
        out.point = (bestA + bestB) * 0.5f;
        return true;
    }

    // ------------------------------------------------------- Triangle テスト関数

    namespace
    {
        // 三角形上の最近傍点を Voronoi 領域分類で求める
        math::Vector3 ClosestPointOnTriangle(const math::Vector3& p, const Triangle& tri)
        {
            const math::Vector3& a = tri.v[0];
            const math::Vector3& b = tri.v[1];
            const math::Vector3& c = tri.v[2];

            const math::Vector3 ab = b - a;
            const math::Vector3 ac = c - a;
            const math::Vector3 ap = p - a;

            const float d1 = math::Vector3::Dot(ab, ap);
            const float d2 = math::Vector3::Dot(ac, ap);
            if (d1 <= 0.0f && d2 <= 0.0f) return a;

            const math::Vector3 bp = p - b;
            const float d3 = math::Vector3::Dot(ab, bp);
            const float d4 = math::Vector3::Dot(ac, bp);
            if (d3 >= 0.0f && d4 <= d3) return b;

            const float vc = d1 * d4 - d3 * d2;
            if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
            {
                const float v = d1 / (d1 - d3);
                return a + ab * v;
            }

            const math::Vector3 cp2 = p - c;
            const float d5 = math::Vector3::Dot(ab, cp2);
            const float d6 = math::Vector3::Dot(ac, cp2);
            if (d6 >= 0.0f && d5 <= d6) return c;

            const float vb = d5 * d2 - d1 * d6;
            if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
            {
                const float w = d2 / (d2 - d6);
                return a + ac * w;
            }

            const float va = d3 * d6 - d5 * d4;
            if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
            {
                const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
                return b + (c - b) * w;
            }

            const float denom = 1.0f / (va + vb + vc);
            const float v = vb * denom;
            const float w = vc * denom;
            return a + ab * v + ac * w;
        }
    } // anonymous namespace

    bool PhysicsSolver::TestSphereTriangle(const SphereCollider& s,
                                            const Triangle& tri,
                                            ContactPoint& out)
    {
        const math::Vector3 center  = s.GetAABB().Center();
        const math::Vector3 closest = ClosestPointOnTriangle(center, tri);
        const math::Vector3 diff    = center - closest;
        const float         distSq  = diff.LengthSq();

        if (distSq >= s.m_radius * s.m_radius) return false;

        const float dist = std::sqrt(distSq);

        if (dist < 1e-6f)
        {
            // 球中心が三角形面上またはほぼ一致: 面法線を使用
            // 双面: 球中心が裏側なら法線を反転
            math::Vector3 n = tri.normal;
            if (math::Vector3::Dot(n, center - tri.v[0]) < 0.0f) n = -n;
            out.normal = n;
            out.depth  = s.m_radius;
        }
        else
        {
            out.normal = diff * (1.0f / dist);
            out.depth  = s.m_radius - dist;
        }

        out.point = closest;
        return true;
    }

    bool PhysicsSolver::TestAABBTriangle(const AABBCollider& b,
                                          const Triangle& tri,
                                          ContactPoint& out)
    {
        // SAT (Separating Axis Theorem): 13 軸をテストする
        // 軸: 3 面法線 (AABB 軸) + 3 辺 × 3 AABB 軸 = 9 + 1 三角形法線 = 13
        const AABB&         aabb = b.GetAABB();
        const math::Vector3 center = aabb.Center();
        const math::Vector3 half   = aabb.Extents();

        // 三角形頂点を AABB 中心相対座標に変換
        math::Vector3 v[3];
        for (int i = 0; i < 3; ++i) v[i] = tri.v[i] - center;

        math::Vector3 e[3];
        e[0] = v[1] - v[0];
        e[1] = v[2] - v[1];
        e[2] = v[0] - v[2];

        // AABB の 3 軸 (X, Y, Z)
        const math::Vector3 aabbAxes[3] = {
            {1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f}
        };

        float minOverlap = std::numeric_limits<float>::max();
        math::Vector3 bestAxis;

        constexpr float EPS = 1e-6f;

        auto TestAxis = [&](math::Vector3 axis) -> bool
        {
            const float lenSq = axis.LengthSq();
            if (lenSq < EPS) return true; // 縮退軸はスキップ (分離なし扱い)
            axis = axis * (1.0f / std::sqrt(lenSq));

            // AABB の投影半幅
            const float r = half.x * std::abs(axis.x)
                          + half.y * std::abs(axis.y)
                          + half.z * std::abs(axis.z);

            // 三角形の投影範囲
            const float p0 = math::Vector3::Dot(v[0], axis);
            const float p1 = math::Vector3::Dot(v[1], axis);
            const float p2 = math::Vector3::Dot(v[2], axis);
            const float triMin = std::min({p0, p1, p2});
            const float triMax = std::max({p0, p1, p2});

            const float overlap = std::min(r - triMin, triMax + r);
            if (overlap <= 0.0f) return false; // 分離軸発見

            if (overlap < minOverlap)
            {
                minOverlap = overlap;
                bestAxis   = axis;
            }
            return true;
        };

        // 3 AABB 軸
        for (int i = 0; i < 3; ++i)
            if (!TestAxis(aabbAxes[i])) return false;

        // 三角形法線
        if (!TestAxis(tri.normal)) return false;

        // 9 クロス積軸 (edgeI × aabbAxisJ)
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                if (!TestAxis(math::Vector3::Cross(e[i], aabbAxes[j]))) return false;

        // 全軸で重なり → 衝突
        // 法線方向: AABB 中心 → 三角形 重心 に対して bestAxis を合わせる
        const math::Vector3 triCentroid = (v[0] + v[1] + v[2]) / 3.0f;
        if (math::Vector3::Dot(bestAxis, triCentroid) < 0.0f)
            bestAxis = -bestAxis;

        out.normal = -bestAxis; // AABB → 三角形 方向
        out.depth  = minOverlap;
        out.point  = center + bestAxis * (half.x + half.y + half.z) / 3.0f; // 近似接触点
        return true;
    }

    bool PhysicsSolver::TestCapsuleTriangle(const CapsuleCollider& c,
                                             const Triangle& tri,
                                             ContactPoint& out)
    {
        const math::Vector3 segS = c.GetSegmentStart();
        const math::Vector3 segE = c.GetSegmentEnd();

        // カプセル線分と三角形の各エッジの最近傍点ペアを探す
        float   bestDistSq = std::numeric_limits<float>::max();
        math::Vector3 bestCapsule, bestTriPt;

        // エッジ 3 本との線分-線分最近傍
        for (int k = 0; k < 3; ++k)
        {
            const math::Vector3& ta = tri.v[k];
            const math::Vector3& tb = tri.v[(k + 1) % 3];
            math::Vector3 cp, tp;
            ClosestPointsOnSegments(segS, segE, ta, tb, cp, tp);
            const float dSq = (cp - tp).LengthSq();
            if (dSq < bestDistSq)
            {
                bestDistSq = dSq;
                bestCapsule = cp;
                bestTriPt   = tp;
            }
        }

        // 三角形面への投影点も候補に加える
        // カプセル線分のサンプル点 (3 点) を三角形上の最近傍点と比較
        for (float t : {0.0f, 0.5f, 1.0f})
        {
            const math::Vector3 sample = segS + (segE - segS) * t;
            const math::Vector3 tp     = ClosestPointOnTriangle(sample, tri);
            const float dSq = (sample - tp).LengthSq();
            if (dSq < bestDistSq)
            {
                bestDistSq  = dSq;
                bestCapsule = sample;
                bestTriPt   = tp;
            }
        }

        if (bestDistSq >= c.m_radius * c.m_radius) return false;

        const float dist = std::sqrt(bestDistSq);
        if (dist < 1e-6f)
        {
            math::Vector3 n = tri.normal;
            if (math::Vector3::Dot(n, bestCapsule - tri.v[0]) < 0.0f) n = -n;
            out.normal = n;
            out.depth  = c.m_radius;
        }
        else
        {
            out.normal = (bestCapsule - bestTriPt) * (1.0f / dist);
            out.depth  = c.m_radius - dist;
        }

        out.point = bestTriPt;
        return true;
    }

    // ---------------------------------------------------- ConvexHull テスト関数

    namespace
    {
        // 各形状の GJK サポート関数
        math::Vector3 SupportSphere(const void* shape, const math::Vector3& dir)
        {
            const auto* s = static_cast<const SphereCollider*>(shape);
            const math::Vector3 center = s->GetAABB().Center();
            return center + dir.Normalized() * s->m_radius;
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

        math::Vector3 SupportCapsule(const void* shape, const math::Vector3& dir)
        {
            const auto* c = static_cast<const CapsuleCollider*>(shape);
            const math::Vector3 s = c->GetSegmentStart();
            const math::Vector3 e = c->GetSegmentEnd();
            // セグメント上で dir と最も内積が大きい点 + radius
            const math::Vector3 best = (math::Vector3::Dot(s, dir) >= math::Vector3::Dot(e, dir))
                                        ? s : e;
            return best + dir.Normalized() * c->m_radius;
        }

        // GJK (Simplex 付き) + EPA から ContactPoint を構築するヘルパー
        bool GJKEPAToContact(const void* shapeA, SupportFn fnA,
                             const void* shapeB, SupportFn fnB,
                             ContactPoint& out)
        {
            const GJKResult gjk = GJK_Intersect(shapeA, fnA, shapeB, fnB);
            if (!gjk.intersects) return false;

            const EPAResult epa = EPA_GetContactInfo(shapeA, fnA, shapeB, fnB, gjk.simplex);
            if (!epa.valid) return false;

            out.normal = epa.normal;
            out.depth  = epa.depth;
            out.point  = (epa.contactA + epa.contactB) * 0.5f;
            return true;
        }
    } // anonymous namespace

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

    bool PhysicsSolver::TestCapsuleConvex(const CapsuleCollider& c,
                                           const ConvexHullCollider& hull,
                                           ContactPoint& out)
    {
        return GJKEPAToContact(&c,    SupportCapsule,
                               &hull, ConvexHullCollider::SupportFnImpl, out);
    }

    bool PhysicsSolver::TestSphereTriangleMesh(const SphereCollider& s,
                                                const TriangleMeshCollider& mesh,
                                                ContactPoint& out)
    {
        // BVH を使って候補三角形を絞り込み、最も深い接触点を採用する
        AABB queryAABB;
        const math::Vector3 center = s.GetAABB().Center();
        queryAABB.min = center - math::Vector3{s.m_radius, s.m_radius, s.m_radius};
        queryAABB.max = center + math::Vector3{s.m_radius, s.m_radius, s.m_radius};

        bool    found   = false;
        float   maxDepth = -1.0f;
        ContactPoint best;

        mesh.GetBVH().Query(queryAABB, [&](const Triangle& tri)
        {
            ContactPoint cp;
            if (TestSphereTriangle(s, tri, cp) && cp.depth > maxDepth)
            {
                maxDepth = cp.depth;
                best     = cp;
                found    = true;
            }
        });

        if (found) out = best;
        return found;
    }

    bool PhysicsSolver::TestAABBTriangleMesh(const AABBCollider& b,
                                              const TriangleMeshCollider& mesh,
                                              ContactPoint& out)
    {
        bool    found    = false;
        float   maxDepth = -1.0f;
        ContactPoint best;

        mesh.GetBVH().Query(b.GetAABB(), [&](const Triangle& tri)
        {
            ContactPoint cp;
            if (TestAABBTriangle(b, tri, cp) && cp.depth > maxDepth)
            {
                maxDepth = cp.depth;
                best     = cp;
                found    = true;
            }
        });

        if (found) out = best;
        return found;
    }

    bool PhysicsSolver::TestCapsuleTriangleMesh(const CapsuleCollider& c,
                                                 const TriangleMeshCollider& mesh,
                                                 ContactPoint& out)
    {
        // カプセルの AABB で BVH をクエリ
        const AABB queryAABB = c.GetAABB();
        bool    found    = false;
        float   maxDepth = -1.0f;
        ContactPoint best;

        mesh.GetBVH().Query(queryAABB, [&](const Triangle& tri)
        {
            ContactPoint cp;
            if (TestCapsuleTriangle(c, tri, cp) && cp.depth > maxDepth)
            {
                maxDepth = cp.depth;
                best     = cp;
                found    = true;
            }
        });

        if (found) out = best;
        return found;
    }

} // namespace fbzz::physics
