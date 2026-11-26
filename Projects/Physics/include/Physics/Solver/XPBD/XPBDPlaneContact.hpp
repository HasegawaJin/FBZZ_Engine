/// @file    XPBDPlaneContact.hpp
/// @brief   剛体上の 1 点を無限平面より上に保つ片側拘束
/// @author  Hasegawa Jin
/// @date    2026-09-02

/// @note M1 の足場。M3 で narrowphase が作る接触へ差し替える (Docs/design/active-ragdoll.md)。
/// @note 解く数式は同じで、«どこがどれだけ潜っているか» を誰が決めるかだけが変わる。
#pragma once
#include <Physics/Api.hpp>

#include <Physics/Solver/XPBD/XPBDConstraint.hpp>

namespace fbzz::physics
{
    class XPBDPlaneContact final : public XPBDConstraint
    {
    public:
        /// @param[in] localPoint   剛体ローカルの接触点 (重心からのオフセット)。
        /// @param[in] radius       接触点の太さ [m]。平面から radius だけ浮いた所で止まる。
        /// @param[in] planeNormal  平面の法線 (上向き)。正規化して渡すこと。
        /// @param[in] planeOffset  平面の位置。dot(planeNormal, x) = planeOffset を満たす面。
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

        RigidBody*    m_body = nullptr; ///< 平面との接触を解く剛体の非所有参照。
        math::Vector3 m_localPoint; ///< 剛体ローカルで重心を基準とする接触点 [m]。
        float         m_radius = 0.0f; ///< 接触点から平面へ確保する隙間の半径 [m]。
        math::Vector3 m_normal{ 0.0f, 1.0f, 0.0f }; ///< 平面の許容領域を指すワールド単位法線。
        float         m_offset = 0.0f; ///< 平面方程式 dot(x, normal) の右辺 [m]。
        float         m_friction = 0.0f; ///< 接触点と平面との摩擦係数。

        float m_lambda         = 0.0f; ///< 今のサブステップの法線拘束の累積乗数。
        float m_lambdaFriction = 0.0f; ///< 今のサブステップの摩擦拘束の累積乗数。
        float m_penetration    = 0.0f; ///< 直近のサブステップで測った貫通の深さ [m]。

        math::Vector3 m_previousPoint; ///< 前の substep の位置パスを終えた時点の接触点。ここからの移動が «滑った量»。
        bool          m_hasPreviousPoint = false; ///< 摩擦の計算に使う直前の接触点を記録したかどうか。
    };
} /// @note namespace fbzz::physics
