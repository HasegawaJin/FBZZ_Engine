// FBZZ Engine
// CollisionPair.hpp | fbzz::physics
// BroadPhase が生成する衝突候補ペア
#pragma once
#include <Physics/Collider.hpp>
#include <memory>

namespace fbzz::physics 
{

    struct CollisionPair {
        std::shared_ptr<Collider> colliderA;
        std::shared_ptr<Collider> colliderB;
    };

} // namespace fbzz::physics
