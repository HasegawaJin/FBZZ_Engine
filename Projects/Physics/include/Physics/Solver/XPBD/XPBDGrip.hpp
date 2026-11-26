/// @file    XPBDGrip.hpp
/// @brief   外部剛体を再積分しない、有限出力・破断付きの把持拘束。
/// @author  Hasegawa Jin
/// @date    2026-11-03
#pragma once
#include <Physics/Solver/XPBD/XPBDConstraint.hpp>
#include <memory>

namespace fbzz::physics {
enum class XPBDGripStatus { NONE, HOLDING, RELEASED, FORCE_LIMIT, DISTANCE_LIMIT, TARGET_LOST };
/// @note Hz・無次元減衰比・N・N·m・m。破断条件0は無効。回転上限0は回転保持を停止する。
struct XPBDGripSettings {
    float frequency = 8.0f; ///< 把持ばねの固有周波数 [Hz]。
    float dampingRatio = 1.0f; ///< 把持ばねの減衰比。1 は臨界減衰。
    float maxForce = 1000.0f; ///< アンカーを近づける力の上限 [N]。
    float maxTorque = 100.0f; ///< 相対姿勢を保持するトルクの上限 [N·m]。0 なら回転保持を行わない。
    float breakForce = 2000.0f; ///< 並進の要求力がこの値 [N] を超えたら解除する。0 なら無効。
    float breakDistance = 0.5f; ///< アンカー間がこの距離 [m] を超えて離れたら解除する。0 なら無効。
    float maxReach = 0.35f; ///< 把持開始時に許す剛体重心から把持点までの距離 [m]。
    bool constrainRotation = false; ///< 把持開始時の相対姿勢を保持するかどうか。
};
class XPBDGrip final : public XPBDConstraint {
public:
    /// @note point は両側共通のワールド把持点。other=null は固定点。失敗時は現在の把持を変更しない。
    [[nodiscard]] FBZZ_PHYSICS_API bool Bind(RigidBody* body, RigidBody* other,
        const math::Vector3& point, const XPBDGripSettings& settings);
    void Release()
    {
        m_status = XPBDGripStatus::RELEASED;
        ResetLambda();
    }
    void ResetLambda() override
    {
        m_force = 0.0f;
        m_impulse = math::Vector3::ZERO;
        m_angularImpulse = math::Vector3::ZERO;
    }
    FBZZ_PHYSICS_API void SolvePosition(float h) override;
    FBZZ_PHYSICS_API void SolveVelocity(float h) override;
    [[nodiscard]] FBZZ_PHYSICS_API XPBDGripStatus Status() const;
    [[nodiscard]] FBZZ_PHYSICS_API math::Vector3 Anchor() const;
    [[nodiscard]] FBZZ_PHYSICS_API math::Vector3 Target() const;
    [[nodiscard]] float Force() const { return m_force; }
private:
    [[nodiscard]] bool Alive() const { return !m_bodyLife.expired() && (!m_other || !m_otherLife.expired()); }
    RigidBody* m_body = nullptr; ///< 姿勢を補正する把持側の剛体。非所有。
    RigidBody* m_other = nullptr; ///< 反作用を返す相手の剛体。非所有。nullptr はワールド固定。
    std::weak_ptr<const void> m_bodyLife; ///< 把持側の破棄を検知する寿命トークン。
    std::weak_ptr<const void> m_otherLife; ///< 相手の破棄を検知する寿命トークン。
    math::Vector3 m_localA; ///< 把持側の剛体ローカルのアンカー [m]。
    math::Vector3 m_localB; ///< 相手側の剛体ローカルのアンカー [m]。固定点ではワールド座標。
    math::Vector3 m_impulse; ///< 位置パスで蓄積した並進力積 [N·s]。
    math::Vector3 m_angularImpulse; ///< 位置パスで蓄積した角力積 [N·m·s]。
    math::Vector3 m_reactionPoint; ///< 速度パスで反作用の力積を返すワールド位置 [m]。
    math::Quaternion m_rotation = math::Quaternion::Identity(); ///< 把持開始時の相手に対する姿勢。固定点ではワールド姿勢。
    XPBDGripSettings m_settings; ///< 把持の強さ、動作範囲と解除条件。
    XPBDGripStatus m_status = XPBDGripStatus::NONE; ///< 現在の把持状態または解除した理由。
    float m_force = 0.0f; ///< 直近の位置パスで実際に加えた力の大きさ [N]。
};
} // namespace fbzz::physics
