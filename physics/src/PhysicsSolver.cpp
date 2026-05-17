// FBZZ Engine
// PhysicsSolver.cpp | fbzz::physics
// 衝突検出 (Broad/Narrow フェーズ) と衝突解決 (インパルスベース)
#include <physics/PhysicsSolver.hpp>
#include <physics/PhysicsMaterial.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::physics 
{

    // ------------------------------------------------------------------ BroadPhase
    void PhysicsSolver::BroadPhase(const std::vector<std::shared_ptr<RigidBody>>& bodies,
                                    std::vector<CollisionPair>& outPairs)
    {
        for (size_t i = 0; i < bodies.size(); ++i)
        {
            if (!bodies[i]->GetCollider()) continue;
            for (size_t j = i + 1; j < bodies.size(); ++j)
            {
                if (!bodies[j]->GetCollider()) continue;
                if (bodies[i]->IsStatic() && bodies[j]->IsStatic()) continue;
                if (bodies[i]->GetCollider()->GetAABB()
                        .Overlaps(bodies[j]->GetCollider()->GetAABB()))
                {
                    outPairs.push_back({ bodies[i]->GetCollider(),
                                        bodies[j]->GetCollider() });
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
            ColliderType tA = pair.colliderA->GetType();
            ColliderType tB = pair.colliderB->GetType();
            ContactPoint cp;
            bool hit = false;

            if (tA == ColliderType::SPHERE && tB == ColliderType::SPHERE)
            {
                hit = TestSphereSphere(
                    *static_cast<SphereCollider*>(pair.colliderA.get()),
                    *static_cast<SphereCollider*>(pair.colliderB.get()), cp);
            }
            else if (tA == ColliderType::AABB && tB == ColliderType::AABB)
            {
                hit = TestAABBAABB(
                    *static_cast<AABBCollider*>(pair.colliderA.get()),
                    *static_cast<AABBCollider*>(pair.colliderB.get()), cp);
            }
            else if (tA == ColliderType::SPHERE && tB == ColliderType::AABB)
            {
                hit = TestSphereAABB(
                    *static_cast<SphereCollider*>(pair.colliderA.get()),
                    *static_cast<AABBCollider*> (pair.colliderB.get()), cp);
            }
            else if (tA == ColliderType::AABB && tB == ColliderType::SPHERE)
            {
                // 引数順を正規化して呼び、法線を反転する
                hit = TestSphereAABB(
                    *static_cast<SphereCollider*>(pair.colliderB.get()),
                    *static_cast<AABBCollider*> (pair.colliderA.get()), cp);
                if (hit)
                {
                    cp.normal  = -cp.normal;
                    std::swap(cp.bodyA, cp.bodyB);
                }
            }
            // Capsule 組み合わせは後回し実装時に追加

            if (hit) outContacts.push_back(cp);
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
        if (bodyA && bodyB)
            e = PhysicsMaterial::CombineRestitution(bodyA->m_material, bodyB->m_material);

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
        out.bodyA  = a.m_body;
        out.bodyB  = b.m_body;
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
        out.bodyA = a.m_body;
        out.bodyB = b.m_body;
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
        out.bodyA = s.m_body;
        out.bodyB = b.m_body;
        return true;
    }

    bool PhysicsSolver::TestSphereCapsule(const SphereCollider& /*s*/,
                                        const CapsuleCollider& /*c*/,
                                        ContactPoint& /*out*/)
    {
        return false; // CapsuleCollider 後回し実装時に追加
    }

} // namespace fbzz::physics
