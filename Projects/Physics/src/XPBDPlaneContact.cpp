/// @file    XPBDPlaneContact.cpp
/// @brief   剛体上の 1 点を無限平面より上に保つ片側拘束
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <Physics/XPBDPlaneContact.hpp>

namespace fbzz::physics
{
    XPBDPlaneContact::XPBDPlaneContact(RigidBody*           body,
                                       const math::Vector3& localPoint,
                                       float                radius,
                                       const math::Vector3& planeNormal,
                                       float                planeOffset)
        : m_body(body)
        , m_localPoint(localPoint)
        , m_radius(radius)
        , m_normal(planeNormal.NormalizedOr(math::Vector3::UP))
        , m_offset(planeOffset)
    {
    }

    void XPBDPlaneContact::SolvePosition(float h)
    {
        m_penetration = 0.0f;
        if (!m_body) return;

        const math::Vector3 r     = m_body->GetRotation() * m_localPoint;
        const math::Vector3 world = m_body->GetPosition() + r;
        const float depth = m_offset + m_radius - math::Vector3::Dot(m_normal, world);

        // 片側拘束。離れている間は λ も動かさない ─ 引き戻す力を持たせると床が
        // 磁石になり、跳ねずに貼り付く。
        if (depth <= 0.0f) return;
        m_penetration = depth;

        // SolvePositional は correction の «逆» へ動かすので、法線の逆向きに深さを渡す。
        SolvePositional(m_body, nullptr, r, math::Vector3::ZERO,
                        m_normal * -depth, 0.0f, h, m_lambda);
    }
} // namespace fbzz::physics
