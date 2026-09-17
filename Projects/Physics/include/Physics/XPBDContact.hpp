/// @file    XPBDContact.hpp
/// @brief   NarrowPhase が作った接触点 1 個を substep の中で解く拘束
/// @author  Hasegawa Jin
/// @date    2026-09-03
///
/// @note 接触の «検出» を自前で書かない理由: "Small Steps in Physics Simulation"
///       (Macklin 2019) の主張は解法ではなく刻みの話で、接触の作り方を変える必要はない。
///       既存の PhysicsSolver::NarrowPhase が返す ContactPoint をそのまま拘束に包めば、
///       カプセル vs 三角メッシュも地形も 1 行も書かずに効く。M5 で World 全体を
///       substep 化するときも、変わるのは «誰が NarrowPhase を呼ぶか» だけになる
///       (Docs/design/active-ragdoll.md)。
/// @note 相手を «解く / 解かない» で分ける理由: ラグドールの骨どうしは同じソルバが積分している
///       ので双方を動かしてよい。だが World が積分している瓦礫を substep の中で動かすと
///       同じフレームで 2 回進むことになるため、相手が World 側の剛体のときは «動かない壁»
///       として解き、こちらが受けた反作用を力積として溜めてフレーム末に 1 回だけ返す。
#pragma once

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
        /// @param depth       貫通深さ [m]。正。
        /// @param friction    クーロン摩擦係数。
        /// @param restitution 反発係数 [0,1]。
        void Set(RigidBody*           body,
                 RigidBody*           other,
                 bool                 solveOther,
                 const math::Vector3& worldPoint,
                 const math::Vector3& normal,
                 float                depth,
                 float                friction,
                 float                restitution);

        void ResetLambda() override;
        void SolvePosition(float h) override;
        void SolveVelocity(float h) override;

        /// 解かなかった相手へ返すべき力積 [N·s]。**符号は «相手が受ける» 向き**。
        /// フレーム末に ApplyImpulseAtPoint へそのまま渡す。
        [[nodiscard]] math::Vector3 ReactionImpulse() const { return m_reaction; }
        [[nodiscard]] math::Vector3 ContactPointWorld() const { return m_worldPoint; }
        /// other → body の向き。デバッグ表示が «どちらへ押し返しているか» を出すのに使う。
        [[nodiscard]] math::Vector3 GetNormal() const { return m_normal; }
        [[nodiscard]] RigidBody*    GetOther() const { return m_other; }
        [[nodiscard]] bool          IsOtherSolved() const { return m_solveOther; }
        /// 直前の substep で押し返した深さ [m]。0 なら離れている。
        [[nodiscard]] float GetPenetration() const { return m_penetration; }

    private:
        [[nodiscard]] math::Vector3 AnchorA() const;
        [[nodiscard]] math::Vector3 AnchorB() const;
        void SolveFriction(float h, const math::Vector3& rA, const math::Vector3& rB);

        RigidBody*    m_body  = nullptr;
        RigidBody*    m_other = nullptr;
        bool          m_solveOther = false;

        /// 接触点。body 側は必ずローカル、other 側は «解くならローカル、解かないならワールド»。
        math::Vector3 m_anchorA;
        math::Vector3 m_anchorB;
        math::Vector3 m_worldPoint;
        math::Vector3 m_normal{ 0.0f, 1.0f, 0.0f };
        float         m_depth       = 0.0f;
        float         m_friction    = 0.0f;
        float         m_restitution = 0.0f;

        float         m_lambdaNormal   = 0.0f;
        float         m_lambdaFriction = 0.0f;
        float         m_penetration    = 0.0f;
        /// この substep のはじめ (位置パスより前) の法線相対速度。反発の基準。
        float         m_approachSpeed  = 0.0f;
        math::Vector3 m_reaction;

        /// 前の substep の位置パスを終えた時点の接触点。ここからの差が «滑った量»。
        math::Vector3 m_previousA;
        math::Vector3 m_previousB;
        bool          m_hasPrevious = false;
    };
} // namespace fbzz::physics
