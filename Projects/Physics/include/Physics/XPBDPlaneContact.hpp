/// @file    XPBDPlaneContact.hpp
/// @brief   剛体上の 1 点を無限平面より上に保つ片側拘束
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// M1 の足場。M3 で narrowphase が作る接触へ差し替える (Docs/design/active-ragdoll.md)。
/// 解く数式は同じで、«どこがどれだけ潜っているか» を誰が決めるかだけが変わる。
#pragma once

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
        XPBDPlaneContact(RigidBody*           body,
                         const math::Vector3& localPoint,
                         float                radius,
                         const math::Vector3& planeNormal,
                         float                planeOffset);

        void ResetLambda() override { m_lambda = 0.0f; }
        void SolvePosition(float h) override;

        /// 直前の substep で押し返した深さ [m]。0 なら接触していない。
        [[nodiscard]] float GetPenetration() const { return m_penetration; }

    private:
        RigidBody*    m_body = nullptr;
        math::Vector3 m_localPoint;
        float         m_radius = 0.0f;
        math::Vector3 m_normal{ 0.0f, 1.0f, 0.0f };
        float         m_offset = 0.0f;

        float m_lambda      = 0.0f;
        float m_penetration = 0.0f;
    };
} // namespace fbzz::physics
