/// @file    XPBDBodyDrive.hpp
/// @brief   有限出力の並進・回転ばね減衰ドライブ。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#pragma once
#include <Physics/XPBDConstraint.hpp>

namespace fbzz::physics {
/// @note Hz / 無次元減衰比 / N / N·m。上限または周波数 0 は該当する駆動を無効にする。
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/BlueprintAPI/Utilities/Struct/MakePhysicsControlData 設定の分離を参照。0 出力の契約は FBZZ 独自。
struct XPBDBodyDriveSettings {
    bool enabled = false;
    float linearFrequency = 0.0f;
    float angularFrequency = 4.0f;
    float linearDampingRatio = 1.0f;
    float angularDampingRatio = 1.0f;
    float maxForce = 0.0f;
    float maxTorque = 100.0f;
    float targetVelocityMultiplier = 1.0f;
};

class XPBDBodyDrive final : public XPBDConstraint {
public:
    explicit XPBDBodyDrive(RigidBody* body) : m_body(body) {}
    /// @note null はワールド支持。親を指定すると反作用を親へ返し、目標は親の剛体空間となる。
    void SetReference(RigidBody* parent) { m_parent=parent; }
    void SetSettings(const XPBDBodyDriveSettings& settings) { m_settings=settings; }
    /// @note 並進は重心位置。速度・角速度も参照空間で渡す。アニメーションのサンプル間隔で求める。
    void SetTarget(const math::Vector3& position,const math::Quaternion& rotation,
        const math::Vector3& velocity=math::Vector3::ZERO,const math::Vector3& angular=math::Vector3::ZERO)
    { m_targetPosition=position; m_targetRotation=rotation; m_targetVelocity=velocity; m_targetAngular=angular; }
    [[nodiscard]] bool IsSaturated() const { return m_linearSaturated || m_angularSaturated; }
    void ResetLambda() override { m_linearLambda=0.0f; m_angularLambda=0.0f; m_linearSaturated=false; m_angularSaturated=false; }
    FBZZ_PHYSICS_API void SolvePosition(float h) override;
private:
    RigidBody* m_body=nullptr;
    RigidBody* m_parent=nullptr;
    XPBDBodyDriveSettings m_settings;
    math::Vector3 m_targetPosition;
    math::Quaternion m_targetRotation=math::Quaternion::Identity();
    math::Vector3 m_targetVelocity;
    math::Vector3 m_targetAngular;
    bool m_linearSaturated=false;
    bool m_angularSaturated=false;
    float m_linearLambda=0.0f;
    float m_angularLambda=0.0f;
};
}
