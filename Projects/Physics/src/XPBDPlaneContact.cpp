/// @file    XPBDPlaneContact.cpp
/// @brief   剛体上の 1 点を無限平面より上に保つ片側拘束
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <Physics/XPBDPlaneContact.hpp>

#include <cmath>
#include <algorithm>

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
        m_penetration = 0.0f;
        m_lambda         = 0.0f;
        m_lambdaFriction = 0.0f;
    }

    void XPBDPlaneContact::SolvePosition(float h)
    {
        if (!m_body) return;

        const math::Vector3 r     = m_body->GetRotation() * m_localPoint;
        const math::Vector3 world = m_body->GetPosition() + r;
        const float depth = m_offset + m_radius - math::Vector3::Dot(m_normal, world);

        /// @note 片側拘束。離れている間は λ も動かさない ─ 引き戻す力を持たせると床が
        /// @note 磁石になり、跳ねずに貼り付く。
        if (depth <= 0.0f) {
            m_previousPoint    = world;
            m_hasPreviousPoint = true;
            return;
        }
        m_penetration = std::max(m_penetration,depth);

        /// @note SolvePositional は correction の «逆» へ動かすので、法線の逆向きに深さを渡す。
        SolvePositional(m_body, nullptr, r, math::Vector3::ZERO,
                        m_normal * -depth, 0.0f, h, m_lambda);

        SolveFriction(h);

        m_previousPoint =
            m_body->GetPosition() + m_body->GetRotation() * m_localPoint;
        m_hasPreviousPoint = true;
    }

    /// @note 実速度の法線成分を反発0で解く。幾何的な貫通補正は XPBDSolver が速度再構成から除外する。
    void XPBDPlaneContact::SolveVelocity(float h)
    {
        if (!m_body || m_penetration <= 0.0f || h <= 0.0f) return;

        const math::Vector3 r = m_body->GetRotation() * m_localPoint;
        const math::Vector3 pointVelocity =
            m_body->GetVelocity() + math::Vector3::Cross(m_body->GetAngularVelocity(), r);

        const float normalSpeed = math::Vector3::Dot(m_normal, pointVelocity);
        /// @note 接触は押す力だけを持つ。離反中の法線速度を消すと、床へ引く力と架空の摩擦が生じる。
        /// @see https://box2d.org/posts/2024/02/solver2d/ Projected Gauss-Seidel の片側接触力積。
        if (normalSpeed >= 0.0f) return;
        const float inverseMass=GeneralizedInverseMass(m_body,r,m_normal);
        const float normalImpulse=inverseMass>0.0f ? std::abs(normalSpeed)/inverseMass : 0.0f;

        ApplyVelocityChangeAtPoint(m_body, nullptr, r, math::Vector3::ZERO,
                                   m_normal * normalSpeed);
        /// @note 幾何的な固着は実速度へ混ぜず、速度の摩擦を物理的な法線力積に従わせる。
        const auto velocity=m_body->GetVelocity()+math::Vector3::Cross(m_body->GetAngularVelocity(),r);
        const auto tangent=velocity-m_normal*math::Vector3::Dot(m_normal,velocity);
        if (m_friction>0.0f && normalImpulse>0.0f)
            ApplyVelocityChangeAtPoint(m_body,nullptr,r,math::Vector3::ZERO,tangent,m_friction*normalImpulse);
    }

    /// @note 静摩擦を位置パスで解く。速度パスで «接線速度を削る» 形にすると、倒れた体が
    /// @note 止まった後もじりじり滑る ── 削るのは速度で、既にずれた位置は戻らないため。
    /// @note この substep で滑った距離そのものを打ち消し、上限だけをクーロンに従わせる。
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
} /// @note namespace fbzz::physics
