/// @file    Constraint.cpp
/// @brief   剛体間制約の基底クラス。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#include <Physics/Constraint.hpp>

namespace fbzz::physics
{
    Constraint::Constraint(RigidBody* bodyA, RigidBody* bodyB)
        : m_bodyALifetime(bodyA ? bodyA->m_registrationLifetime.Observe() : std::weak_ptr<const void>{})
        , m_bodyBLifetime(bodyB ? bodyB->m_registrationLifetime.Observe() : std::weak_ptr<const void>{})
        , m_bodyA(bodyA), m_bodyB(bodyB)
    {
    }
} /// @note namespace fbzz::physics
