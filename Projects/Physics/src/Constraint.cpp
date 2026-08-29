/// @file    Constraint.cpp
/// @brief   剛体間制約の基底クラス。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#include <Physics/Constraint.hpp>

namespace fbzz::physics
{
    Constraint::Constraint(RigidBody* bodyA, RigidBody* bodyB)
        : m_bodyA(bodyA), m_bodyB(bodyB)
    {
    }
} // namespace fbzz::physics
