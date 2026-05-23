// FBZZ Engine
// PhysicsSolver.cpp | fbzz::physics
// 衝突検出 (Broad/Narrow フェーズ) と衝突解決 (インパルスベース)
#include <Physics/PhysicsSolver.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include <algorithm>
#include <cmath>

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
                                    std::vector<CollisionPair>& outPairs)
    {
        for (size_t i = 0; i < colliders.size(); ++i)
        {
            if (!colliders[i].collider) continue;
            for (size_t j = i + 1; j < colliders.size(); ++j)
            {
                if (!colliders[j].collider) continue;
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

            if (hit) {
                cp.bodyA = pair.colliderA.body;
                cp.bodyB = pair.colliderB.body;
                cp.colliderA = pair.colliderA.collider.get();
                cp.colliderB = pair.colliderB.collider.get();
                cp.materialA = pair.colliderA.material;
                cp.materialB = pair.colliderB.material;
                cp.isTrigger = pair.colliderA.isTrigger || pair.colliderB.isTrigger;
                outContacts.push_back(cp);
            }
        }
    }

    // --------------------------------------------------------------------- Resolve
    void PhysicsSolver::Resolve(std::vector<ContactPoint>& contacts)
    {
        constexpr int VELOCITY_ITER = 10;
        for (int i = 0; i < VELOCITY_ITER; ++i)
            for (auto& cp : contacts)
                ResolveVelocity(cp);

        for (auto& cp : contacts)
            ResolvePosition(cp);
    }

    // ---------------------------------------------------------- ResolveVelocity
    void PhysicsSolver::ResolveVelocity(ContactPoint& cp)
    {
        if (cp.isTrigger) return;

        RigidBody* bodyA = cp.bodyA;
        RigidBody* bodyB = cp.bodyB;

        const float invMassA = bodyA ? bodyA->GetInvMass() : 0.0f;
        const float invMassB = bodyB ? bodyB->GetInvMass() : 0.0f;

        math::Vector3 vA = bodyA ? bodyA->GetVelocity()        : math::Vector3::ZERO;
        math::Vector3 vB = bodyB ? bodyB->GetVelocity()        : math::Vector3::ZERO;
        math::Vector3 wA = bodyA ? bodyA->GetAngularVelocity() : math::Vector3::ZERO;
        math::Vector3 wB = bodyB ? bodyB->GetAngularVelocity() : math::Vector3::ZERO;

        math::Vector3 rA = bodyA ? cp.point - bodyA->GetPosition() : math::Vector3::ZERO;
        math::Vector3 rB = bodyB ? cp.point - bodyB->GetPosition() : math::Vector3::ZERO;

        math::Vector3 vAContact = vA + math::Vector3::Cross(wA, rA);
        math::Vector3 vBContact = vB + math::Vector3::Cross(wB, rB);
        math::Vector3 vRel      = vAContact - vBContact;
        float         vRelN     = math::Vector3::Dot(vRel, cp.normal);

        if (vRelN > 0.0f) return; // 離反中は解決不要

        float e = 0.3f;
        if (cp.materialA && cp.materialB)
            e = PhysicsMaterial::CombineRestitution(*cp.materialA, *cp.materialB);

        // resting contact ではジッター防止のため反発なし
        constexpr float REST_THRESHOLD = 0.5f;
        if (std::abs(vRelN) < REST_THRESHOLD) e = 0.0f;

        float angTermA = 0.0f;
        float angTermB = 0.0f;
        if (bodyA)
        {
            math::Vector3 rAxN = math::Vector3::Cross(rA, cp.normal);
            angTermA = math::Vector3::Dot(
                math::Vector3::Cross(bodyA->ApplyInvInertia(rAxN), rA), cp.normal);
        }
        if (bodyB)
        {
            math::Vector3 rBxN = math::Vector3::Cross(rB, cp.normal);
            angTermB = math::Vector3::Dot(
                math::Vector3::Cross(bodyB->ApplyInvInertia(rBxN), rB), cp.normal);
        }

        const float denom = invMassA + invMassB + angTermA + angTermB;
        if (denom == 0.0f) return;

        const float j = -(1.0f + e) * vRelN / denom;

        if (bodyA)
        {
            bodyA->SetVelocity(vA + cp.normal * (j * invMassA));
            bodyA->ApplyAngularImpulse(math::Vector3::Cross(rA, cp.normal * j));
        }
        if (bodyB)
        {
            bodyB->SetVelocity(vB - cp.normal * (j * invMassB));
            bodyB->ApplyAngularImpulse(-math::Vector3::Cross(rB, cp.normal * j));
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
        }
        else if (oy <= ox && oy <= oz)
        {
            out.depth  = oy;
            out.normal = { 0.0f, dir.y >= 0.0f ? 1.0f : -1.0f, 0.0f };
        }
        else
        {
            out.depth  = oz;
            out.normal = { 0.0f, 0.0f, dir.z >= 0.0f ? 1.0f : -1.0f };
        }

        out.point = (aabbA.Center() + aabbB.Center()) * 0.5f;
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

} // namespace fbzz::physics
