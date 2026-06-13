// FBZZ Engine
// CollisionPair.hpp | fbzz::physics
// BroadPhase が生成する衝突候補ペア
#pragma once
#include <Physics/Collider.hpp>
#include <Physics/PhysicsMaterial.hpp>

namespace fbzz::physics
{
    class RigidBody;

    // World に登録されるコライダー参照。すべてのフィールドは非所有ポインタ。
    // WHY: BroadPhase で毎 substep 大量に複製されるため、shared_ptr の atomic refcount を
    //      排除して衝突検出のコストを最小化する。所有権は ColliderComponent (unique_ptr) が持つ。
    struct ColliderInstance {
        Collider* collider = nullptr;
        RigidBody* body = nullptr;
        const PhysicsMaterial* material = nullptr;
        math::Vector3 centerOffset = math::Vector3::ZERO;
        bool isTrigger = false;
        int layer = 0;
    };

    // NarrowPhase はこのペアだけを詳細判定する。
    // WHY: m_colliders は NarrowPhase 中に再確保されないため、非所有ポインタで十分。
    struct CollisionPair {
        const ColliderInstance* colliderA = nullptr;
        const ColliderInstance* colliderB = nullptr;
    };

} // namespace fbzz::physics
