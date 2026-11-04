/// @file    FixedConstraint.hpp
/// @brief   2 つの剛体の相対位置・相対回転を固定する溶接拘束。
/// @author  Hasegawa Jin
/// @date    2026-06-08
#pragma once
#include <Physics/Api.hpp>
#include <Physics/Constraint.hpp>

namespace fbzz::physics
{
    class FixedConstraint : public Constraint
    {
    public:
        FBZZ_PHYSICS_API FixedConstraint(RigidBody* bodyA, RigidBody* bodyB);

        FBZZ_PHYSICS_API void SolvePosition(float dt) override;
        ConstraintType GetType() const override { return ConstraintType::FIXED; }

    private:
        math::Vector3    m_localOffsetA;
        math::Quaternion m_relativeRotation;
    };
} /// @note namespace fbzz::physics
