// FBZZ Engine
// SliderConstraint.hpp | fbzz::physics
// 1 軸方向の移動だけを許すプリズマティック拘束
#pragma once
#include <Physics/Constraint.hpp>

namespace fbzz::physics
{
    class SliderConstraint : public Constraint
    {
    public:
        SliderConstraint(RigidBody* bodyA,
                         RigidBody* bodyB,
                         const math::Vector3& axis);

        void SolvePosition(float dt) override;
        ConstraintType GetType() const override { return ConstraintType::SLIDER; }

        void SetLimits(float minDistance, float maxDistance);
        void ClearLimits();

        math::Vector3 m_axis;
        bool  m_limitEnabled = false;
        float m_minDistance = 0.0f;
        float m_maxDistance = 0.0f;

    };
} // namespace fbzz::physics
