// FBZZ Engine
// HingeConstraint.hpp | fbzz::physics
// Pivot anchor hinge constraint
#pragma once
#include <Physics/Constraint.hpp>

namespace fbzz::physics
{
    class HingeConstraint : public Constraint
    {
    public:
        HingeConstraint(RigidBody* bodyA,
                        RigidBody* bodyB,
                        const math::Vector3& localAnchorA,
                        const math::Vector3& localAnchorB,
                        const math::Vector3& axis);

        void SolvePosition(float dt) override;

        math::Vector3 m_localAnchorA;
        math::Vector3 m_localAnchorB;
        math::Vector3 m_axis;
    };
} // namespace fbzz::physics
