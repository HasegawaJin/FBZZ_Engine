/// @file    XPBDSolver.hpp
/// @brief   小さい substep を大量に回す位置ベースの剛体ソルバ
/// @author  Hasegawa Jin
/// @date    2026-09-02

/// @note 大きい質量比の関節連鎖は PGS の反復増加でなく、小さい substep で収束させる。
/// @note ばね・モーターの更新から速度を再構成し、硬い位置補正を速度へ混ぜない。
/// @see Docs/design/active-ragdoll.md
#pragma once
#include <Physics/Api.hpp>

#include <Physics/XPBDConstraint.hpp>

#include <memory>
#include <vector>

namespace fbzz::physics
{
    enum class XPBDProjectionMode { MOTION, LIMIT };
    /// @note 預かった剛体を substep で積分し、拘束を解く。

    /// @note ボディの寿命はこのクラスの外にある。**剛体を破棄する前に RemoveBody /
    /// @note ClearBodies を呼ぶこと** — デストラクタでは触らない (既に壊れている可能性がある)。

    /// @note **同じ剛体を World にも積分させないこと。** World::Step とこのループの両方が
    /// @note 積分すると 1 フレームで 2 回進み、重力が 2 倍かかった上に速度の定義が食い違う。
    /// @note World へは «衝突形状としてだけ» 登録する (静的扱い) か、World から外す。
    class XPBDSolver
    {
    public:
        /// @note 1 フレームを substeps 回に割って解く。
        FBZZ_PHYSICS_API void Step(float dt);
        /// @note 接触の位置解決後、速度を再計算する前に射影する。context はこの呼び出し中のみ参照する。
        FBZZ_PHYSICS_API void Step(float dt, void (*projectPositions)(void*), void* context);
        /// @note prepareContacts は各 substep の積分後、拘束解決前に呼ぶ。接触配列の再構築はこの中で行える。
        FBZZ_PHYSICS_API void Step(float dt, void (*prepareContacts)(void*),
                                  void (*projectPositions)(void*), void* context);
        /// @note LIMIT は姿勢を接触補正の前に制限し、外向きの速度だけを除く。
        /// @param correctionIterations 各 substep で硬い関節と接触を解き直す回数。最小 1。
        FBZZ_PHYSICS_API void Step(float dt, void (*prepareContacts)(void*),
                                  void (*projectPositions)(void*), void* context, XPBDProjectionMode mode,
                                  int correctionIterations = 4);

        /// @note 既定 8。増やすほど硬い拘束が安定するが、線形にコストが増える。
        FBZZ_PHYSICS_API void SetSubsteps(int substeps);
        [[nodiscard]] int GetSubsteps() const { return m_substeps; }

        void SetGravity(const math::Vector3& gravity) { m_gravity = gravity; }
        [[nodiscard]] math::Vector3 GetGravity() const { return m_gravity; }

        /// @brief 積分対象へ加える。同じ剛体を二重に登録しても 1 つとして扱う。
        /// @note Sleep を止める理由: RigidBody は sleep 中に invMass も I⁻¹ も 0 を返す。
        /// @note そのまま拘束へ入れると «動かない剛体» になり、症状は «ラグドールが固まる»
        /// @note で原因が sleep だとは画面から分からない。外部から駆動するボディなので
        /// @note 預かっている間は寝かせず、外すときに元の設定へ戻す。
        FBZZ_PHYSICS_API void AddBody(RigidBody* body);
        FBZZ_PHYSICS_API void RemoveBody(RigidBody* body);
        FBZZ_PHYSICS_API void ClearBodies();
        [[nodiscard]] int GetBodyCount() const { return static_cast<int>(m_bodies.size()); }

        FBZZ_PHYSICS_API void AddConstraint(std::unique_ptr<XPBDConstraint> constraint);
        FBZZ_PHYSICS_API void ClearConstraints();
        [[nodiscard]] int GetConstraintCount() const
        {
            return static_cast<int>(m_constraints.size());
        }

        /// @brief 毎フレーム作り直す拘束 (接触)。**所有しない** ─ 実体は呼び出し側が
        /// @note 使い回す配列で持ち、フレームごとに Clear → Add し直す。
        /// @note 所有する側と分ける理由: 接触は «誰と触れているか» が毎フレーム変わるので、
        /// @note unique_ptr で持つと毎フレーム確保と解放が走る。関節のように寿命が長いものと
        /// @note 同じ器に入れると、ClearConstraints が関節まで落としてしまう。

        /// @note 常に永続拘束の**後**に解かれる。硬いもの (関節) より接触を後に置くと
        /// @note «骨がわずかに伸びてでも床から出る» になり、逆だと足が床へ沈む。
        FBZZ_PHYSICS_API void ClearTransient();
        FBZZ_PHYSICS_API void AddTransient(XPBDConstraint* constraint);
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
            math::Vector3 physicalPosition;
            math::Quaternion physicalRotation;
            math::Vector3 projectionTranslation;
            math::Quaternion projectionRotation;
            /// @note AddBody 時点の m_allowSleeping。外すときに戻す。
            bool             restoreSleeping = true;
        };

        FBZZ_PHYSICS_API void Integrate(BodyState& state, float h) const;
        FBZZ_PHYSICS_API void DeriveVelocity(const BodyState& state, float h) const;
        static void LimitProjectionVelocity(const BodyState& state);

        std::vector<BodyState>                       m_bodies;
        std::vector<std::unique_ptr<XPBDConstraint>> m_constraints;
        /// @note 非所有。呼び出し側が寿命を保証する (接触)。
        std::vector<XPBDConstraint*>                 m_transient;
        math::Vector3                                m_gravity{ 0.0f, -9.81f, 0.0f };
        int                                          m_substeps = 8;
    };
} /// @note namespace fbzz::physics
