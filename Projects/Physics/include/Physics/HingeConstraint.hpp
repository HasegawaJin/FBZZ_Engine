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
        void ApplyForce(float dt) override;
        ConstraintType GetType() const override { return ConstraintType::HINGE; }

        void SetLimits(float minAngleRad, float maxAngleRad);
        void ClearLimits();
        void SetMotor(float targetAngularSpeed, float maxTorque);
        void ClearMotor();

        math::Vector3 m_localAnchorA;
        math::Vector3 m_localAnchorB;
        math::Vector3 m_axis;
        bool  m_limitEnabled = false;
        float m_minAngle = 0.0f;
        float m_maxAngle = 0.0f;
        bool  m_motorEnabled = false;
        float m_motorTargetSpeed = 0.0f;
        float m_motorMaxTorque = 0.0f;

    private:
        math::Vector3 m_referenceA;
        math::Vector3 m_referenceB;
        float CurrentAngle() const;
    };
} // namespace fbzz::physics
