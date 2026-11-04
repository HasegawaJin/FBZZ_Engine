/// @file    DistanceConstraint.hpp
/// @brief   固定距離を保つ剛体ロッド制約。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#pragma once
#include <Physics/Api.hpp>
#include <Physics/Constraint.hpp>

namespace fbzz::physics
{
    /// @note 2 つの剛体間距離を m_distance に保つ。伸縮しない棒の簡易表現。
    class DistanceConstraint : public Constraint
    {
    public:
        FBZZ_PHYSICS_API DistanceConstraint(RigidBody* bodyA, RigidBody* bodyB, float distance);

        FBZZ_PHYSICS_API void SolvePosition(float dt) override;
        ConstraintType GetType() const override { return ConstraintType::DISTANCE; }

        float m_distance = 1.0f;
    };
} /// @note namespace fbzz::physics
