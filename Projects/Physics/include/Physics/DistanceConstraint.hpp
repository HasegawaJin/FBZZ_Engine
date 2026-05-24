// FBZZ Engine
// DistanceConstraint.hpp | fbzz::physics
// 固定距離を保つ剛体ロッド制約
#pragma once
#include <Physics/Constraint.hpp>

namespace fbzz::physics
{
    // 2 つの剛体間距離を m_distance に保つ。伸縮しない棒の簡易表現。
    class DistanceConstraint : public Constraint
    {
    public:
        DistanceConstraint(RigidBody* bodyA, RigidBody* bodyB, float distance);

        void SolvePosition(float dt) override;
        ConstraintType GetType() const override { return ConstraintType::DISTANCE; }

        float m_distance = 1.0f;
    };
} // namespace fbzz::physics
