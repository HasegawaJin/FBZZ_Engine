/// @file    HingeConstraint.hpp
/// @brief   ピボット点を共有する簡易ヒンジ制約。
/// @author  Hasegawa Jin
/// @date    2026-05-22
#pragma once
#include <Physics/Api.hpp>
#include <Physics/Constraints/Constraint.hpp>

namespace fbzz::physics
{
    /// @brief 2 つのローカルアンカーを同じワールド位置へ寄せる。
    /// @note 軸はデバッグ表示と将来の角度制限用に保持する。
    class HingeConstraint : public Constraint
    {
    public:
        FBZZ_PHYSICS_API HingeConstraint(RigidBody* bodyA,
                        RigidBody* bodyB,
                        const math::Vector3& localAnchorA,
                        const math::Vector3& localAnchorB,
                        const math::Vector3& axis);

        FBZZ_PHYSICS_API void SolvePosition(float dt) override;
        FBZZ_PHYSICS_API void ApplyForce(float dt) override;
        ConstraintType GetType() const override { return ConstraintType::HINGE; }

        FBZZ_PHYSICS_API void SetLimits(float minAngleRad, float maxAngleRad);
        FBZZ_PHYSICS_API void ClearLimits();
        FBZZ_PHYSICS_API void SetMotor(float targetAngularSpeed, float maxTorque);
        FBZZ_PHYSICS_API void ClearMotor();

        math::Vector3 m_localAnchorA; ///< 回転軸が通る A の剛体ローカルの点 [m]。
        math::Vector3 m_localAnchorB; ///< 回転軸が通る B の剛体ローカルの点 [m]。
        math::Vector3 m_axis; ///< ヒンジが回転を許す軸。
        bool  m_limitEnabled = false; ///< ヒンジの角度を上下限内に制限するかどうか。
        float m_minAngle = 0.0f; ///< ヒンジの基準姿勢からの回転下限 [rad]。
        float m_maxAngle = 0.0f; ///< ヒンジの基準姿勢からの回転上限 [rad]。
        bool  m_motorEnabled = false; ///< ヒンジ軸まわりの速度を駆動するかどうか。
        float m_motorTargetSpeed = 0.0f; ///< ヒンジ軸まわりの目標角速度 [rad/s]。
        float m_motorMaxTorque = 0.0f; ///< モーターが加えるトルクの絶対値の上限 [N·m]。

    private:
        math::Vector3 m_referenceA; ///< A の剛体ローカルで回転角の原点を定める参照方向。
        math::Vector3 m_referenceB; ///< B の剛体ローカルで回転角の原点を定める参照方向。
        FBZZ_PHYSICS_API float CurrentAngle() const;
    };
} /// @note namespace fbzz::physics
