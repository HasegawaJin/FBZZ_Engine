/// @file    XPBDPoseAnchor.hpp
/// @brief   剛体 1 個を目標のワールド姿勢へ引き止める拘束
/// @author  Hasegawa Jin
/// @date    2026-09-03
///
/// @note 要る理由: 関節は «隣の骨との相対» しか拘束しない。根の骨は何にも繋がっていないので、
///       サーボが形を保ったまま全体が重力で落ち、立っている間ずっと走らせると胴が床下へ沈む。
///       «無負荷ならクリップそのもの» を成立させるには、根をアニメーションへ繋ぎ止める
///       1 本が要る。完全に固定すると «押されて沈む» が手足のたわみだけになるため、
///       compliance と力の上限を持たせ、関節のサーボと同じ «力負けする» 仕組みを根にも効かせる。
#pragma once

#include <Physics/XPBDConstraint.hpp>

namespace fbzz::physics
{
    class XPBDPoseAnchor final : public XPBDConstraint
    {
    public:
        explicit XPBDPoseAnchor(RigidBody* body) : m_body(body) {}

        /// false で «繋ぎ止めない»。脱力して倒れる間は切る。
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        [[nodiscard]] bool IsEnabled() const { return m_enabled; }

        void SetTarget(const math::Vector3& position, const math::Quaternion& rotation)
        {
            m_targetPosition = position;
            m_targetRotation = rotation;
        }

        /// @param maxForce  出せる力の上限 [N]。0 以下で無制限。
        /// @param maxTorque 出せるトルクの上限 [N·m]。0 以下で無制限。
        /// @param sag       上限の力を出し切るまでに沈む距離 [m]。
        /// @param tilt      上限のトルクを出し切るまでに傾く角 [rad]。
        void SetStrength(float maxForce, float maxTorque, float sag, float tilt);
        /// 有限出力用。0 以下の軸は解かない。単位は SetStrength と同じ。
        void SetFiniteStrength(float maxForce, float maxTorque, float sag, float tilt);

        void ResetLambda() override;
        void SolvePosition(float h) override;

        /// 直前の substep で目標からどれだけ離れていたか [m]。
        [[nodiscard]] float GetOffset() const { return m_offset; }
        /// 力・トルクの上限に張り付いているか ＝ 引き止め切れていない。
        [[nodiscard]] bool IsSaturated() const { return m_saturated; }

    private:
        RigidBody*       m_body = nullptr;
        bool             m_enabled = false;
        bool             m_finiteStrength = false;
        math::Vector3    m_targetPosition;
        math::Quaternion m_targetRotation = math::Quaternion::Identity();

        float m_maxForce  = 0.0f;
        float m_maxTorque = 0.0f;
        float m_linearCompliance  = 0.0f;
        float m_angularCompliance = 0.0f;

        float m_lambdaLinear  = 0.0f;
        float m_lambdaAngular = 0.0f;
        float m_offset    = 0.0f;
        bool  m_saturated = false;
    };
} // namespace fbzz::physics
