/// @file    XPBDContact.cpp
/// @brief   接触点 1 個を substep の中で解く拘束
/// @author  Hasegawa Jin
/// @date    2026-09-03
#include <Physics/XPBDContact.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::physics
{
    namespace
    {
        /// これ以下の接近速度では跳ねない。入れないと、床に乗っているだけの剛体が
        /// 数値誤差ぶんの «接近» を毎 substep 跳ね返して細かく震える。
        constexpr float kRestitutionThreshold = 0.5f;

        math::Vector3 PointVelocity(const RigidBody* body, const math::Vector3& r)
        {
            if (!body) return math::Vector3::ZERO;
            return body->GetVelocity() + math::Vector3::Cross(body->GetAngularVelocity(), r);
        }
    } // namespace

    void XPBDContact::Set(RigidBody*           body,
                          RigidBody*           other,
                          bool                 solveOther,
                          const math::Vector3& worldPoint,
                          const math::Vector3& normal,
                          float                depth,
                          float                friction,
                          float                restitution)
    {
        m_body       = body;
        m_other      = other;
        m_solveOther = solveOther && other != nullptr;
        m_worldPoint = worldPoint;
        m_normal     = normal.NormalizedOr(math::Vector3::UP);
        m_depth      = depth;
        m_friction   = friction;
        m_restitution = restitution;

        /// @note 接触点を «それぞれの剛体に貼り付けて» 覚える。substep のあいだに姿勢が変われば
        ///       貼り付けた点も動くので、«どこが触れているか» を毎 substep 作り直さずに済む。
        m_anchorA = body
            ? body->GetRotation().Inverse() * (worldPoint - body->GetPosition())
            : worldPoint;
        m_anchorB = m_solveOther
            ? other->GetRotation().Inverse() * (worldPoint - other->GetPosition())
            : worldPoint;

        m_lambdaNormal   = 0.0f;
        m_lambdaFriction = 0.0f;
        m_penetration    = 0.0f;
        m_approachSpeed  = 0.0f;
        m_reaction       = math::Vector3::ZERO;
        m_hasPrevious    = false;
    }

    math::Vector3 XPBDContact::AnchorA() const
    {
        return m_body ? m_body->GetPosition() + m_body->GetRotation() * m_anchorA : m_anchorA;
    }

    math::Vector3 XPBDContact::AnchorB() const
    {
        return m_solveOther
            ? m_other->GetPosition() + m_other->GetRotation() * m_anchorB
            : m_anchorB;
    }

    void XPBDContact::ResetLambda()
    {
        m_lambdaNormal   = 0.0f;
        m_lambdaFriction = 0.0f;
    }

    void XPBDContact::SolvePosition(float h)
    {
        m_penetration = 0.0f;
        if (!m_body) return;

        const math::Vector3 pA = AnchorA();
        const math::Vector3 pB = AnchorB();
        const math::Vector3 rA = pA - m_body->GetPosition();
        const math::Vector3 rB = m_solveOther ? pB - m_other->GetPosition() : math::Vector3::ZERO;

        /// @note 生成時は pA == pB。法線方向へ離れたぶんだけ貫通が減る。
        const float penetration = m_depth - math::Vector3::Dot(m_normal, pA - pB);

        /// @note 反発の基準になる «ぶつかった勢い» は、位置を直す前にしか取れない。
        ///       相手が World 側でも速度は読む (瓦礫にぶつかられたら跳ねてよい)。
        const math::Vector3 relative =
            PointVelocity(m_body, rA) -
            PointVelocity(m_other, m_other ? pB - m_other->GetPosition() : math::Vector3::ZERO);
        m_approachSpeed = math::Vector3::Dot(m_normal, relative);

        if (penetration <= 0.0f) {
            m_previousA   = pA;
            m_previousB   = pB;
            m_hasPrevious = true;
            return;
        }
        m_penetration = penetration;

        RigidBody* solvedOther = m_solveOther ? m_other : nullptr;
        /// @note SolvePositional は correction の «逆» へ A を動かすので、法線の逆向きに渡す。
        SolvePositional(m_body, solvedOther, rA, rB,
                        m_normal * -penetration, 0.0f, h, m_lambdaNormal);

        SolveFriction(h, rA, rB);

        m_previousA   = AnchorA();
        m_previousB   = AnchorB();
        m_hasPrevious = true;
    }

    /// 静摩擦。この substep で接線方向へ滑った距離をそのまま打ち消し、上限だけを
    /// クーロン (|λ_t| ≤ μ|λ_n|) に従わせる。速度パスで削る形にすると、倒れた体が
    /// 止まった後もじりじり滑る ── 削れるのは速度で、既にずれた位置は戻らないため。
    void XPBDContact::SolveFriction(float h, const math::Vector3& rA, const math::Vector3& rB)
    {
        if (m_friction <= 0.0f || !m_hasPrevious) return;

        math::Vector3 slip = (AnchorA() - m_previousA) - (AnchorB() - m_previousB);
        slip -= m_normal * math::Vector3::Dot(m_normal, slip);

        SolvePositional(m_body, m_solveOther ? m_other : nullptr, rA, rB,
                        slip, 0.0f, h, m_lambdaFriction,
                        m_friction * std::abs(m_lambdaNormal));
    }

    void XPBDContact::SolveVelocity(float h)
    {
        if (!m_body || m_penetration <= 0.0f || h <= 0.0f) return;

        const math::Vector3 pA = AnchorA();
        const math::Vector3 pB = AnchorB();
        const math::Vector3 rA = pA - m_body->GetPosition();
        const math::Vector3 rB = m_solveOther ? pB - m_other->GetPosition() : math::Vector3::ZERO;
        RigidBody* solvedOther = m_solveOther ? m_other : nullptr;

        /// @note 相対速度は «動かさない相手» の分も含めて測る。壁として解く相手でも、それが
        ///       動いていれば «ぶつけられた» 側の速度差が本物になる。変えるのはこちら側だけ。
        const math::Vector3 relative =
            PointVelocity(m_body, rA) -
            PointVelocity(m_other, m_other ? pB - m_other->GetPosition() : math::Vector3::ZERO);
        const float normalSpeed = math::Vector3::Dot(m_normal, relative);

        /// @note 法線力積。位置パスの λ を刻みで割ると、この substep で押し返した強さになる。
        const float normalImpulse = std::abs(m_lambdaNormal) / h;

        /// @note 動摩擦。接線速度を «法線力積 × μ» の範囲だけ削る。位置パスの静摩擦で
        ///       止まり切っている間はここに残る接線速度が無いので、二重には効かない。
        const math::Vector3 tangential = relative - m_normal * normalSpeed;
        if (m_friction > 0.0f)
            ApplyVelocityChangeAtPoint(m_body, solvedOther, rA, rB,
                                       tangential, m_friction * normalImpulse);

        /// @note 反発。位置パスは «めり込みを消す» だけで跳ね返さないので、跳ねさせたければ
        ///       進入時の速度をここで作り直す。閾値以下は跳ねない (床の上での震え止め)。
        if (m_restitution > 0.0f && m_approachSpeed < -kRestitutionThreshold) {
            const float target = -m_restitution * m_approachSpeed;
            if (target > normalSpeed) {
                ApplyVelocityChangeAtPoint(m_body, solvedOther, rA, rB,
                                           m_normal * (normalSpeed - target));
            }
        }

        /// @note 解かなかった相手が受け取るべき反作用。法線は other → body の向きなので、
        ///       相手は逆向きに押される。摩擦ぶんは足さない — 瓦礫を «蹴る» のはほぼ法線方向の
        ///       押しで接線側は桁が 1 つ下がり、法線だけの方が調整しやすい。
        if (m_other && !m_solveOther)
            m_reaction += m_normal * -normalImpulse;
    }
} // namespace fbzz::physics
