/// @file    XPBDPlaneContact.hpp
/// @brief   剛体上の 1 点を無限平面より上に保つ片側拘束
/// @author  Hasegawa Jin
/// @date    2026-09-02

/// @note M1 の足場。M3 で narrowphase が作る接触へ差し替える (Docs/design/active-ragdoll.md)。
/// @note 解く数式は同じで、«どこがどれだけ潜っているか» を誰が決めるかだけが変わる。
#pragma once
#include <Physics/Api.hpp>

#include <Physics/XPBDConstraint.hpp>

namespace fbzz::physics
{
    class XPBDPlaneContact final : public XPBDConstraint
    {
    public:
        /// @param localPoint   剛体ローカルの接触点 (重心からのオフセット)。
        /// @param radius       接触点の太さ [m]。平面から radius だけ浮いた所で止まる。
        /// @param planeNormal  平面の法線 (上向き)。正規化して渡すこと。
        /// @param planeOffset  平面の位置。dot(planeNormal, x) = planeOffset を満たす面。
        FBZZ_PHYSICS_API XPBDPlaneContact(RigidBody*           body,
                         const math::Vector3& localPoint,
                         float                radius,
                         const math::Vector3& planeNormal,
                         float                planeOffset);

        /// @note 面を動かす。歩いているキャラクターの足元へ床を付いて回らせるために使う。
        FBZZ_PHYSICS_API void SetPlane(const math::Vector3& planeNormal, float planeOffset);
        /// @note クーロン摩擦係数。0 で «氷の上»。
        void SetFriction(float friction) { m_friction = friction; }
        /// @note 剛体を «瞬間移動» させた後に呼ぶ。前フレームからの移動を «滑った量» と
        /// @note 見なすので、忘れると捕獲した瞬間に摩擦が跳ねる。
        void ResetHistory() { m_hasPreviousPoint = false; }

        FBZZ_PHYSICS_API void ResetLambda() override;
        FBZZ_PHYSICS_API void SolvePosition(float h) override;
        void SolvePhysicalPosition(float) override {}
        void SolveCorrectionPosition(float h) override { SolvePosition(h); }
        FBZZ_PHYSICS_API void SolveVelocity(float h) override;

        /// @note 直前の substep で押し返した深さ [m]。0 なら接触していない。
        [[nodiscard]] float GetPenetration() const { return m_penetration; }

    private:
        FBZZ_PHYSICS_API void SolveFriction(float h);

        RigidBody*    m_body = nullptr;
        math::Vector3 m_localPoint;
        float         m_radius = 0.0f;
        math::Vector3 m_normal{ 0.0f, 1.0f, 0.0f };
        float         m_offset = 0.0f;
        float         m_friction = 0.0f;

        float m_lambda         = 0.0f;
        float m_lambdaFriction = 0.0f;
        float m_penetration    = 0.0f;

        /// @note 前の substep の位置パスを終えた時点の接触点。ここからの移動が «滑った量»。
        math::Vector3 m_previousPoint;
        bool          m_hasPreviousPoint = false;
    };
} /// @note namespace fbzz::physics
