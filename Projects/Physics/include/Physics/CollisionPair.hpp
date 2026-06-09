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

    // World に登録されるコライダー参照。RigidBody と Material は非所有ポインタで紐づける。
    struct ColliderInstance {
        std::shared_ptr<Collider> collider;
        RigidBody* body = nullptr;
        const PhysicsMaterial* material = nullptr;
        math::Vector3 centerOffset = math::Vector3::ZERO;
        bool isTrigger = false;
        int layer = 0;
    };

    // NarrowPhase はこのペアだけを詳細判定する。
    // WHY: BroadPhase は毎 substep 大量の候補を作るため、ColliderInstance を値コピーすると
    //      内部の shared_ptr 参照カウント更新が衝突検出以外の CPU 負荷になる。
    //      m_colliders は NarrowPhase 中に再確保されないため、非所有ポインタで十分。
    struct CollisionPair {
        const ColliderInstance* colliderA = nullptr;
        const ColliderInstance* colliderB = nullptr;
    };

} // namespace fbzz::physics
