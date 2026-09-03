/// @file    XPBDPlaneContact.cpp
/// @brief   剛体上の 1 点を無限平面より上に保つ片側拘束
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <Physics/XPBDPlaneContact.hpp>

#include <cmath>

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

    void XPBDPlaneContact::SetPlane(const math::Vector3& planeNormal, float planeOffset)
    {
        m_normal = planeNormal.NormalizedOr(math::Vector3::UP);
        m_offset = planeOffset;
    }

    void XPBDPlaneContact::ResetLambda()
    {
        m_lambda         = 0.0f;
        m_lambdaFriction = 0.0f;
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
        if (depth <= 0.0f) {
            m_previousPoint    = world;
            m_hasPreviousPoint = true;
            return;
        }
        m_penetration = depth;

        // SolvePositional は correction の «逆» へ動かすので、法線の逆向きに深さを渡す。
        SolvePositional(m_body, nullptr, r, math::Vector3::ZERO,
                        m_normal * -depth, 0.0f, h, m_lambda);

        SolveFriction(h);

        m_previousPoint =
            m_body->GetPosition() + m_body->GetRotation() * m_localPoint;
        m_hasPreviousPoint = true;
    }

    // 静摩擦を位置パスで解く。速度パスで «接線速度を削る» 形にすると、倒れた体が
    // 止まった後もじりじり滑る ── 削るのは速度で、既にずれた位置は戻らないため。
    // この substep で滑った距離そのものを打ち消し、上限だけをクーロンに従わせる。
    void XPBDPlaneContact::SolveFriction(float h)
    {
        if (m_friction <= 0.0f || !m_hasPreviousPoint) return;

        const math::Vector3 r     = m_body->GetRotation() * m_localPoint;
        const math::Vector3 world = m_body->GetPosition() + r;

        math::Vector3 slip = world - m_previousPoint;
        slip -= m_normal * math::Vector3::Dot(m_normal, slip);

        SolvePositional(m_body, nullptr, r, math::Vector3::ZERO,
                        slip, 0.0f, h, m_lambdaFriction,
                        m_friction * std::abs(m_lambda));
    }
} // namespace fbzz::physics
