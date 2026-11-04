/// @file    XPBDGrip.hpp
/// @brief   外部剛体を再積分しない、有限出力・破断付きの把持拘束。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#pragma once
#include <Physics/XPBDConstraint.hpp>
#include <memory>

namespace fbzz::physics {
enum class XPBDGripStatus { NONE, HOLDING, RELEASED, FORCE_LIMIT, DISTANCE_LIMIT, TARGET_LOST };
/// @note Hz・無次元減衰比・N・N·m・m。破断条件0は無効。回転上限0は回転保持を停止する。
struct XPBDGripSettings {
    float frequency=8.0f;
    float dampingRatio=1.0f;
    float maxForce=1000.0f;
    float maxTorque=100.0f;
    float breakForce=2000.0f;
    float breakDistance=0.5f;
    float maxReach=0.35f;
    bool constrainRotation=false;
};
class XPBDGrip final : public XPBDConstraint {
public:
    /// @note point は両側共通のワールド把持点。other=null は固定点。失敗時は現在の把持を変更しない。
    [[nodiscard]] FBZZ_PHYSICS_API bool Bind(RigidBody* body,RigidBody* other,const math::Vector3& point,const XPBDGripSettings& settings);
    void Release() { m_status=XPBDGripStatus::RELEASED; ResetLambda(); }
    void ResetLambda() override { m_force=0.0f; m_impulse=math::Vector3::ZERO; m_angularImpulse=math::Vector3::ZERO; }
    FBZZ_PHYSICS_API void SolvePosition(float h) override;
    FBZZ_PHYSICS_API void SolveVelocity(float h) override;
    [[nodiscard]] FBZZ_PHYSICS_API XPBDGripStatus Status() const;
    [[nodiscard]] FBZZ_PHYSICS_API math::Vector3 Anchor() const;
    [[nodiscard]] FBZZ_PHYSICS_API math::Vector3 Target() const;
    [[nodiscard]] float Force() const { return m_force; }
private:
    [[nodiscard]] bool Alive() const { return !m_bodyLife.expired() && (!m_other || !m_otherLife.expired()); }
    RigidBody* m_body=nullptr;
    RigidBody* m_other=nullptr;
    std::weak_ptr<const void> m_bodyLife,m_otherLife;
    math::Vector3 m_localA,m_localB;
    math::Vector3 m_impulse,m_angularImpulse,m_reactionPoint;
    math::Quaternion m_rotation=math::Quaternion::Identity();
    XPBDGripSettings m_settings;
    XPBDGripStatus m_status=XPBDGripStatus::NONE;
    float m_force=0.0f;
};
}
