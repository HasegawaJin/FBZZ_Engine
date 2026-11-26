/// @file    XPBDBodyDrive.hpp
/// @brief   有限出力の並進・回転ばね減衰ドライブ。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#pragma once
#include <Physics/Solver/XPBD/XPBDConstraint.hpp>

namespace fbzz::physics {
/// @note Hz / 無次元減衰比 / N / N·m。上限または周波数 0 は該当する駆動を無効にする。
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/BlueprintAPI/Utilities/Struct/MakePhysicsControlData 設定の分離を参照。0 出力の契約は FBZZ 独自。
struct XPBDBodyDriveSettings {
    bool enabled = false; ///< 姿勢ドライブを有効にするかどうか。
    float linearFrequency = 0.0f; ///< 並進ばねの固有周波数 [Hz]。
    float angularFrequency = 4.0f; ///< 回転ばねの固有周波数 [Hz]。
    float linearDampingRatio = 1.0f; ///< 並進の減衰比。1 は臨界減衰。
    float angularDampingRatio = 1.0f; ///< 回転の減衰比。1 は臨界減衰。
    float maxForce = 0.0f; ///< 並進ドライブの力の上限 [N]。0 なら並進を駆動しない。
    float maxTorque = 100.0f; ///< 姿勢ドライブのトルク上限 [N·m]。0 なら回転を駆動しない。
    float targetVelocityMultiplier = 1.0f; ///< 目標の速度と角速度へ掛ける追従倍率。
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
    {
        m_targetPosition=position;
        m_targetRotation=rotation;
        m_targetVelocity=velocity;
        m_targetAngular=angular;
    }
    [[nodiscard]] bool IsSaturated() const { return m_linearSaturated || m_angularSaturated; }
    void ResetLambda() override {
        m_linearLambda=0.0f;
        m_angularLambda=0.0f;
        m_linearSaturated=false;
        m_angularSaturated=false;
    }
    FBZZ_PHYSICS_API void SolvePosition(float h) override;
private:
    RigidBody* m_body=nullptr; ///< ドライブする剛体の非所有参照。
    RigidBody* m_parent=nullptr; ///< 目標の座標系を定め、反作用を受ける剛体の非所有参照。nullptr はワールド基準。
    XPBDBodyDriveSettings m_settings; ///< ドライブの有効範囲、強さと減衰の設定。
    math::Vector3 m_targetPosition; ///< 参照座標系での剛体重心の目標位置 [m]。
    math::Quaternion m_targetRotation=math::Quaternion::Identity(); ///< 参照座標系での剛体の目標姿勢。
    math::Vector3 m_targetVelocity; ///< 参照座標系での目標の並進速度 [m/s]。
    math::Vector3 m_targetAngular; ///< 参照座標系での目標の角速度 [rad/s]。
    bool m_linearSaturated=false; ///< 並進ドライブがこの反復で出力上限に達したかどうか。
    bool m_angularSaturated=false; ///< 回転ドライブがこの反復で出力上限に達したかどうか。
    float m_linearLambda=0.0f; ///< 今のサブステップで蓄積した並進拘束の乗数。
    float m_angularLambda=0.0f; ///< 今のサブステップで蓄積した回転拘束の乗数。
};
}
