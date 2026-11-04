/// @file    RopeConstraint.hpp
/// @brief   最大距離のみを拘束するロープ制約。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#pragma once
#include <Physics/Api.hpp>
#include <Physics/Constraint.hpp>

namespace fbzz::physics
{
    /// @brief m_maxLength 以下では何もしないため、たるみを表現できる制約。
    class RopeConstraint : public Constraint
    {
    public:
        FBZZ_PHYSICS_API RopeConstraint(RigidBody* bodyA, RigidBody* bodyB, float maxLength);

        FBZZ_PHYSICS_API void SolvePosition(float dt) override;
        ConstraintType GetType() const override { return ConstraintType::ROPE; }

        float m_maxLength = 1.0f;
    };
} /// @note namespace fbzz::physics
