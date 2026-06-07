// FBZZ Engine
// FixedConstraint.hpp | fbzz::physics
// 2 つの剛体の相対位置・相対回転を固定する溶接拘束
#pragma once
#include <Physics/Constraint.hpp>

namespace fbzz::physics
{
    class FixedConstraint : public Constraint
    {
    public:
        FixedConstraint(RigidBody* bodyA, RigidBody* bodyB);

        void SolvePosition(float dt) override;
        ConstraintType GetType() const override { return ConstraintType::FIXED; }

    private:
        math::Vector3    m_localOffsetA;
        math::Quaternion m_relativeRotation;
    };
} // namespace fbzz::physics
