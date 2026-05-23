// FBZZ Engine
// PhysicsSolver.hpp | fbzz::physics
// 衝突検出 (Broad/Narrow フェーズ) と衝突解決 (インパルスベース)
#pragma once
#include <functional>
#include <Physics/ContactPoint.hpp>
#include <Physics/CollisionPair.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/AABBCollider.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/RigidBody.hpp>
#include <vector>
#include <memory>

namespace fbzz::physics 
{

    class PhysicsSolver 
    {
    public:
        void BroadPhase(const std::vector<ColliderInstance>& colliders,
                        std::vector<CollisionPair>& outPairs,
                        const std::function<bool(int, int)>& layerFilter);

        void NarrowPhase(const std::vector<CollisionPair>& pairs,
                        std::vector<ContactPoint>& outContacts);

        void Resolve(std::vector<ContactPoint>& contacts);

    private:
        bool TestSphereSphere(const SphereCollider& a, const SphereCollider& b,
                            ContactPoint& out);
        bool TestAABBAABB    (const AABBCollider&   a, const AABBCollider&   b,
                            ContactPoint& out);
        bool TestSphereAABB  (const SphereCollider& s, const AABBCollider&   b,
                            ContactPoint& out);
        bool TestSphereCapsule(const SphereCollider& s, const CapsuleCollider& c,
                            ContactPoint& out);
        bool TestAABBCapsule(const AABBCollider& b, const CapsuleCollider& c,
                            ContactPoint& out);
        bool TestCapsuleCapsule(const CapsuleCollider& a, const CapsuleCollider& b,
                            ContactPoint& out);

        void ResolveVelocity(ContactPoint& cp);
        void ResolvePosition(ContactPoint& cp);

        static constexpr float SLOP      = 0.01f;
        static constexpr float BAUMGARTE = 0.8f;
    };

} // namespace fbzz::physics
