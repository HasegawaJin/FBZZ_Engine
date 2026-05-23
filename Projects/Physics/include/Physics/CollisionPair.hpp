// FBZZ Engine
// CollisionPair.hpp | fbzz::physics
// BroadPhase が生成する衝突候補ペア
#pragma once
#include <Physics/Collider.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include <memory>

namespace fbzz::physics 
{
    class RigidBody;

    struct ColliderInstance {
        std::shared_ptr<Collider> collider;
        RigidBody* body = nullptr;
        const PhysicsMaterial* material = nullptr;
        bool isTrigger = false;
        int layer = 0;
    };

    struct CollisionPair {
        ColliderInstance colliderA;
        ColliderInstance colliderB;
    };

} // namespace fbzz::physics
