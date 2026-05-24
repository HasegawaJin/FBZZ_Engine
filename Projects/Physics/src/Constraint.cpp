// FBZZ Engine
// Constraint.cpp | fbzz::physics
// 剛体間制約の基底クラス
#include <Physics/Constraint.hpp>

namespace fbzz::physics
{
    Constraint::Constraint(RigidBody* bodyA, RigidBody* bodyB)
        : m_bodyA(bodyA), m_bodyB(bodyB)
    {
    }
} // namespace fbzz::physics
