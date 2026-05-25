// FBZZ Engine
// RopeConstraint.hpp | fbzz::physics
// 最大距離のみを拘束するロープ制約
#pragma once
#include <Physics/Constraint.hpp>

namespace fbzz::physics
{
    // m_maxLength 以下では何もしないため、たるみを表現できる。
    class RopeConstraint : public Constraint
    {
    public:
        RopeConstraint(RigidBody* bodyA, RigidBody* bodyB, float maxLength);

        void SolvePosition(float dt) override;
        ConstraintType GetType() const override { return ConstraintType::ROPE; }

        float m_maxLength = 1.0f;
    };
} // namespace fbzz::physics
