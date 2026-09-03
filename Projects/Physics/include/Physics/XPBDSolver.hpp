/// @file    XPBDSolver.hpp
/// @brief   小さい substep を大量に回す位置ベースの剛体ソルバ
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// WHY 既存の PhysicsSolver と別に置くか:
///   PGS + Warm Starting は接触には十分だが、質量比の大きい関節連鎖を «硬いドライブで
///   支える» と反復数を増やしても発散する。反復を増やすより刻みを小さくする方が
///   収束が良いので、substep を主役にしたループを別に持つ。将来 World::Step 全体を
///   このループへ寄せる想定で、そのとき接触もここへ入る (Docs/design/active-ragdoll.md M5)。
///
/// WHY 速度を積分で運ばず «位置から出し直す» か:
///   位置を直接動かして拘束を満たすと、動かした分が速度に反映されないまま次の
///   substep へ行き、拘束が «押し戻す仕事» を毎回やり直す。substep の末尾で
///   v = (x - x_prev)/h と定義し直せば、拘束が動かした分がそのまま速度になる。
#pragma once

#include <Physics/XPBDConstraint.hpp>

#include <memory>
#include <vector>

namespace fbzz::physics
{
    /// 預かった剛体を substep で積分し、拘束を解く。
    ///
    /// ボディの寿命はこのクラスの外にある。**剛体を破棄する前に RemoveBody /
    /// ClearBodies を呼ぶこと** — デストラクタでは触らない (既に壊れている可能性がある)。
    ///
    /// **同じ剛体を World にも積分させないこと。** World::Step とこのループの両方が
    /// 積分すると 1 フレームで 2 回進み、重力が 2 倍かかった上に速度の定義が食い違う。
    /// World へは «衝突形状としてだけ» 登録する (静的扱い) か、World から外す。
    class XPBDSolver
    {
    public:
        /// 1 フレームを substeps 回に割って解く。
        void Step(float dt);

        /// 既定 8。増やすほど硬い拘束が安定するが、線形にコストが増える。
        void SetSubsteps(int substeps);
        [[nodiscard]] int GetSubsteps() const { return m_substeps; }

        void SetGravity(const math::Vector3& gravity) { m_gravity = gravity; }
        [[nodiscard]] math::Vector3 GetGravity() const { return m_gravity; }

        /// 積分対象へ加える。同じ剛体を二重に登録しても 1 つとして扱う。
        ///
        /// WHY Sleep を止めるか: RigidBody は sleep 中に invMass も I⁻¹ も 0 を返す。
        ///     そのまま拘束へ入れると «動かない剛体» になり、症状は «ラグドールが
        ///     固まる» で、原因が sleep だとは画面から分からない。外部から駆動する
        ///     ボディなので預かっている間は寝かせず、外すときに元の設定へ戻す。
        void AddBody(RigidBody* body);
        void RemoveBody(RigidBody* body);
        void ClearBodies();
        [[nodiscard]] int GetBodyCount() const { return static_cast<int>(m_bodies.size()); }

        void AddConstraint(std::unique_ptr<XPBDConstraint> constraint);
        void ClearConstraints();
        [[nodiscard]] int GetConstraintCount() const
        {
            return static_cast<int>(m_constraints.size());
        }

        /// 毎フレーム作り直す拘束 (接触)。**所有しない** ─ 実体は呼び出し側が
        /// 使い回す配列で持ち、フレームごとに Clear → Add し直す。
        ///
        /// WHY 所有する側と分けるか: 接触は «誰と触れているか» が毎フレーム変わるので、
        ///     unique_ptr で持つと毎フレーム確保と解放が走る。関節のように寿命が
        ///     長いものと同じ器に入れると、ClearConstraints が関節まで落としてしまう。
        ///
        /// 常に永続拘束の**後**に解かれる。硬いもの (関節) より接触を後に置くと
        /// «骨がわずかに伸びてでも床から出る» になり、逆だと足が床へ沈む。
        void ClearTransient();
        void AddTransient(XPBDConstraint* constraint);
        [[nodiscard]] int GetTransientCount() const
        {
            return static_cast<int>(m_transient.size());
        }

    private:
        struct BodyState
        {
            RigidBody*       body = nullptr;
            math::Vector3    prevPosition;
            math::Quaternion prevRotation;
            /// AddBody 時点の m_allowSleeping。外すときに戻す。
            bool             restoreSleeping = true;
        };

        void Integrate(BodyState& state, float h) const;
        void DeriveVelocity(const BodyState& state, float h) const;

        std::vector<BodyState>                       m_bodies;
        std::vector<std::unique_ptr<XPBDConstraint>> m_constraints;
        /// 非所有。呼び出し側が寿命を保証する (接触)。
        std::vector<XPBDConstraint*>                 m_transient;
        math::Vector3                                m_gravity{ 0.0f, -9.81f, 0.0f };
        int                                          m_substeps = 8;
    };
} // namespace fbzz::physics
