// FBZZ Engine
// ContactPoint.hpp | fbzz::physics
// NarrowPhase が生成する衝突接触点データ
#pragma once
#include <math/Vector3.hpp>

namespace fbzz::physics 
{

    class RigidBody;

    // World が bodies を生存管理するため raw pointer で保持する
    struct ContactPoint {
        math::Vector3 point;   // 衝突点 (ワールド座標)
        math::Vector3 normal;  // b → a 方向の法線
        float         depth;   // 貫通深度 (正の値)
        RigidBody*    bodyA = nullptr;
        RigidBody*    bodyB = nullptr;
    };

} // namespace fbzz::physics
