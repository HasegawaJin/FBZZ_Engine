// FBZZ Engine
// HingeConstraint.hpp | fbzz::physics
// ピボット点を共有する簡易ヒンジ制約
#pragma once
#include <Physics/Constraint.hpp>

namespace fbzz::physics
{
    // 2 つのローカルアンカーを同じワールド位置へ寄せる。軸はデバッグ表示と将来の角度制限用に保持する。
    class HingeConstraint : public Constraint
    {
    public:
        HingeConstraint(RigidBody* bodyA,
                        RigidBody* bodyB,
                        const math::Vector3& localAnchorA,
                        const math::Vector3& localAnchorB,
                        const math::Vector3& axis);

        void SolvePosition(float dt) override;
        ConstraintType GetType() const override { return ConstraintType::HINGE; }

        math::Vector3 m_localAnchorA;
        math::Vector3 m_localAnchorB;
        math::Vector3 m_axis;
    };
} // namespace fbzz::physics
