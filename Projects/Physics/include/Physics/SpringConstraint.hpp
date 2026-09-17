/// @file    SpringConstraint.hpp
/// @brief   Hooke 則バネ制約。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#pragma once
#include <Physics/Constraint.hpp>

namespace fbzz::physics
{
    /// @brief 位置補正ではなく力として解くため、揺れや減衰を含む柔らかい接続に使う Hooke 則バネ。
    class SpringConstraint : public Constraint
    {
    public:
        SpringConstraint(RigidBody* bodyA,
                         RigidBody* bodyB,
                         float restLength,
                         float stiffness,
                         float damping);

        void ApplyForce(float dt) override;
        ConstraintType GetType() const override { return ConstraintType::SPRING; }

        float m_restLength = 1.0f;
        float m_stiffness = 10.0f;
        float m_damping = 0.5f;
    };
} // namespace fbzz::physics
