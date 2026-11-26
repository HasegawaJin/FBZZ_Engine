/// @file    XPBDPoseAnchor.hpp
/// @brief   剛体 1 個を目標のワールド姿勢へ引き止める拘束
/// @author  Hasegawa Jin
/// @date    2026-09-03

/// @note 要る理由: 関節は «隣の骨との相対» しか拘束しない。根の骨は何にも繋がっていないので、
/// @note サーボが形を保ったまま全体が重力で落ち、立っている間ずっと走らせると胴が床下へ沈む。
/// @note «無負荷ならクリップそのもの» を成立させるには、根をアニメーションへ繋ぎ止める
/// @note 1 本が要る。完全に固定すると «押されて沈む» が手足のたわみだけになるため、
/// @note compliance と力の上限を持たせ、関節のサーボと同じ «力負けする» 仕組みを根にも効かせる。
#pragma once
#include <Physics/Api.hpp>

#include <Physics/Solver/XPBD/XPBDConstraint.hpp>

namespace fbzz::physics
{
    class XPBDPoseAnchor final : public XPBDConstraint
    {
    public:
        explicit XPBDPoseAnchor(RigidBody* body) : m_body(body) {}

        /// @note false で «繋ぎ止めない»。脱力して倒れる間は切る。
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        [[nodiscard]] bool IsEnabled() const { return m_enabled; }

        void SetTarget(const math::Vector3& position, const math::Quaternion& rotation)
        {
            m_targetPosition = position;
            m_targetRotation = rotation;
        }

        /// @param[in] maxForce  出せる力の上限 [N]。0 以下で無制限。
        /// @param[in] maxTorque 出せるトルクの上限 [N·m]。0 以下で無制限。
        /// @param[in] sag       上限の力を出し切るまでに沈む距離 [m]。
        /// @param[in] tilt      上限のトルクを出し切るまでに傾く角 [rad]。
        FBZZ_PHYSICS_API void SetStrength(float maxForce, float maxTorque, float sag, float tilt);
        /// @note 有限出力用。0 以下の軸は解かない。単位は SetStrength と同じ。
        FBZZ_PHYSICS_API void SetFiniteStrength(float maxForce, float maxTorque, float sag, float tilt);

        FBZZ_PHYSICS_API void ResetLambda() override;
        FBZZ_PHYSICS_API void SolvePosition(float h) override;

        /// @note 直前の substep で目標からどれだけ離れていたか [m]。
        [[nodiscard]] float GetOffset() const { return m_offset; }
        /// @note 力・トルクの上限に張り付いているか ＝ 引き止め切れていない。
        [[nodiscard]] bool IsSaturated() const { return m_saturated; }

    private:
        RigidBody*       m_body = nullptr; ///< 目標姿勢へ近づける剛体の非所有参照。
        bool             m_enabled = false; ///< 姿勢アンカーの解決を行うかどうか。
        bool             m_finiteStrength = false; ///< 位置補正に有限出力の計算を使うかどうか。
        math::Vector3    m_targetPosition; ///< アンカーを誘導するワールド目標位置 [m]。
        math::Quaternion m_targetRotation = math::Quaternion::Identity(); ///< アンカーを誘導するワールド目標姿勢。

        float m_maxForce  = 0.0f; ///< 並進補正で課す力の上限 [N]。
        float m_maxTorque = 0.0f; ///< 回転補正で課すトルク上限 [N·m]。
        float m_linearCompliance  = 0.0f; ///< 並進拘束の柔らかさ [m/N]。
        float m_angularCompliance = 0.0f; ///< 回転拘束の柔らかさ [rad/(N·m)]。

        float m_lambdaLinear  = 0.0f; ///< 並進拘束で蓄積した乗数。
        float m_lambdaAngular = 0.0f; ///< 回転拘束で蓄積した乗数。
        float m_offset    = 0.0f; ///< 位置補正を加える前の剛体重心とワールド目標位置の距離 [m]。
        bool  m_saturated = false; ///< 補正に必要な出力が設定上限に達したかどうか。
    };
} /// @note namespace fbzz::physics
