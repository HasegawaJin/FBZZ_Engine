/// @file    XPBDContact.hpp
/// @brief   NarrowPhase が作った接触点 1 個を substep の中で解く拘束
/// @author  Hasegawa Jin
/// @date    2026-09-03

/// @note NarrowPhase が返す接触点を使い、検出方法は既存 PhysicsSolver と共有する。
/// @note 同じ XPBD solver の剛体同士は双方を動かす。World 側は姿勢を再積分せず、双方の有効質量で求めた反作用を substep 境界で返す。
/// @see Docs/design/active-ragdoll.md
#pragma once
#include <Physics/Api.hpp>

#include <Physics/XPBDConstraint.hpp>

namespace fbzz::physics
{
    class XPBDContact final : public XPBDConstraint
    {
    public:
        /// @param body        法線 (+normal) の向きへ押し出される剛体。null にはしない。
        /// @param other       相手。null で «動かない世界» (静的コライダー)。
        /// @param solveOther  true で相手も動かす。false なら反作用を力積として溜める。
        /// @param worldPoint  接触点 (ワールド)。
        /// @param normal      other → body の向き。正規化して渡すこと。
        /// @param depth       符号付き貫通深さ [m]。負は近接した予測接触。
        /// @param friction    クーロン摩擦係数。
        /// @param restitution 反発係数 [0,1]。
        FBZZ_PHYSICS_API void Set(RigidBody*           body,
                 RigidBody*           other,
                 bool                 solveOther,
                 const math::Vector3& worldPoint,
                 const math::Vector3& normal,
                 float                depth,
                 float                friction,
                 float                restitution);

        /// @note 同じ連続接触の固着点だけを引き継ぐ。法線 λ や衝撃速度は持ち越さない。
        FBZZ_PHYSICS_API void ContinueFrictionFrom(const XPBDContact& previous);
        void SetStaticFriction(float friction) { m_staticFriction=friction; }
        /// @pre slop は有限かつ非負の m。近接点の法線拘束を維持する数値許容。
        void SetContactSlop(float slop) { m_contactSlop=slop>0.0f ? slop : 0.0f; }
        [[nodiscard]] bool IsTouching() const { return m_separation<=m_contactSlop; }
        FBZZ_PHYSICS_API void ResetLambda() override;
        FBZZ_PHYSICS_API void SolvePosition(float h) override;
        void SolvePhysicalPosition(float) override {}
        void SolveCorrectionPosition(float h) override { SolvePosition(h); }
        FBZZ_PHYSICS_API void SolveVelocity(float h) override;


        /// @note 解かなかった相手へ返すべき力積 [N·s]。**符号は «相手が受ける» 向き**。
        /// @note 次の substep が相手の更新済み速度を読む前に ApplyImpulseAtPoint へ一度だけ渡す。
        [[nodiscard]] math::Vector3 ReactionImpulse() const { return m_reaction; }
        /// @note 未反映の反作用を消費する。返値を相手へ反映してから次の速度パスを呼ぶ。
        [[nodiscard]] math::Vector3 TakeReactionImpulse() {
            const auto impulse=m_reaction;
            m_reaction=math::Vector3::ZERO;
            return impulse;
        }
        [[nodiscard]] math::Vector3 ContactPointWorld() const { return m_worldPoint; }
        /// @note other → body の向き。デバッグ表示が «どちらへ押し返しているか» を出すのに使う。
        [[nodiscard]] math::Vector3 GetNormal() const { return m_normal; }
        [[nodiscard]] RigidBody*    GetBody() const { return m_body; }
        [[nodiscard]] RigidBody*    GetOther() const { return m_other; }
        [[nodiscard]] bool          IsOtherSolved() const { return m_solveOther; }
        /// @note 直前の substep の貫通深さ [m]。0 は貫通なし。接触許容内かは IsTouching で判定する。
        [[nodiscard]] float GetPenetration() const { return m_penetration; }
        /// @note 候補のワールド姿勢が相手との貫通や継続した静的接触の滑りを増やすかを調べる。
        /// @param additionalSlip 姿勢復帰のため許容する追加の接線変位 [m]。
        /// @param smoothSlipTolerance 接触許容幅を固着点の残差へ連続的に加える。
        [[nodiscard]] FBZZ_PHYSICS_API bool AllowsPoseCorrection(const math::Vector3& position,
            const math::Quaternion& rotation, float additionalSlip, bool smoothSlipTolerance) const;

    private:
        [[nodiscard]] FBZZ_PHYSICS_API math::Vector3 AnchorA() const;
        [[nodiscard]] FBZZ_PHYSICS_API math::Vector3 AnchorB() const;
        FBZZ_PHYSICS_API void CaptureApproachSpeed();
        FBZZ_PHYSICS_API void SolveFriction(float h, const math::Vector3& rA, const math::Vector3& rB);

        RigidBody*    m_body  = nullptr;
        RigidBody*    m_other = nullptr;
        bool          m_solveOther = false;

        /// @note 接触点。body 側は必ずローカル、other 側は «解くならローカル、解かないならワールド»。
        math::Vector3 m_anchorA;
        math::Vector3 m_anchorB;
        math::Vector3 m_worldPoint;
        math::Vector3 m_normal{ 0.0f, 1.0f, 0.0f };
        float         m_depth       = 0.0f;
        float         m_separation = 0.0f;
        float         m_contactSlop = 0.0f;
        float         m_friction    = 0.0f;
        float         m_staticFriction = 0.0f;
        math::Vector3 m_stickA;
        math::Vector3 m_stickB;
        bool m_continuous = false;
        bool m_staticHeld = false;
        float         m_restitution = 0.0f;

        float         m_lambdaNormal   = 0.0f;
        float         m_lambdaFriction = 0.0f;
        float         m_penetration    = 0.0f;
        /// @note この substep のはじめ (位置パスより前) の法線相対速度。反発の基準。
        float         m_approachSpeed  = 0.0f;
        bool m_hasApproachSpeed = false;
        /// @note substep 内の速度力積 [N·s]。反復の差分でなく累積値を法線・摩擦円錐へ射影する。
        float m_velocityNormal = 0.0f;
        math::Vector3 m_velocityFriction = math::Vector3::ZERO;
        math::Vector3 m_reaction;


    };
} /// @note namespace fbzz::physics
