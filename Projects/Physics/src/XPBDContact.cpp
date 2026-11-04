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
        /// @note これ以下の接近速度では跳ねない。入れないと、床に乗っているだけの剛体が
        /// @note 数値誤差ぶんの «接近» を毎 substep 跳ね返して細かく震える。
        constexpr float kRestitutionThreshold = 0.5f;

        math::Vector3 PointVelocity(const RigidBody* body, const math::Vector3& r)
        {
            if (!body) return math::Vector3::ZERO;
            return body->GetVelocity() + math::Vector3::Cross(body->GetAngularVelocity(), r);
        }
    } /// @note namespace

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
        m_separation = -depth;
        m_contactSlop = 0.0f;
        m_friction   = friction;
        m_staticFriction = friction;
        m_continuous = false;
        m_staticHeld = false;
        m_restitution = restitution;

        /// @note 接触点を «それぞれの剛体に貼り付けて» 覚える。substep のあいだに姿勢が変われば
        /// @note 貼り付けた点も動くので、«どこが触れているか» を毎 substep 作り直さずに済む。
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
        m_hasApproachSpeed = false;
        m_velocityNormal = 0.0f;
        m_velocityFriction = math::Vector3::ZERO;
        m_reaction       = math::Vector3::ZERO;
        m_stickA = m_anchorA;
        m_stickB = other ? other->GetRotation().Inverse()*(worldPoint-other->GetPosition()) : worldPoint;
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

    bool XPBDContact::AllowsPoseCorrection(const math::Vector3& position, const math::Quaternion& rotation,
                                           float additionalSlip, bool smoothSlipTolerance) const
    {
        if (!m_body) return true;
        const auto otherPoint = AnchorB();
        const float current = math::Vector3::Dot(m_normal, AnchorA() - otherPoint) - m_depth;
        const float candidate = math::Vector3::Dot(m_normal, position + rotation * m_anchorA - otherPoint) - m_depth;
        if (candidate < std::min(current, 0.0f) - m_contactSlop) return false;
        if (!m_continuous || m_staticFriction <= 0.0f || current > m_contactSlop) return true;

        /// @note 継続した支持では、姿勢制限が静摩擦の固着点から接線方向へ足を引き離さないようにする。
        const auto stickOther = m_other ? m_other->GetPosition() + m_other->GetRotation() * m_stickB : m_stickB;
        const auto tangentialDistanceSquared = [&](const math::Vector3& point) {
            const auto offset = point - stickOther;
            return (offset - m_normal * math::Vector3::Dot(m_normal, offset)).LengthSq();
        };
        const float currentSlip = tangentialDistanceSquared(m_body->GetPosition() + m_body->GetRotation() * m_stickA);
        const float slopSquared = m_contactSlop * m_contactSlop;
        const float stickToleranceSquared = smoothSlipTolerance ? currentSlip + slopSquared : std::max(currentSlip, slopSquared);
        const float recoveryToleranceSquared = additionalSlip * (2.0f * std::sqrt(stickToleranceSquared) + additionalSlip);
        return tangentialDistanceSquared(position + rotation * m_stickA)
            <= stickToleranceSquared + recoveryToleranceSquared;
    }

    void XPBDContact::ContinueFrictionFrom(const XPBDContact& previous)
    {
        if (m_body!=previous.m_body || m_other!=previous.m_other || m_solveOther!=previous.m_solveOther
            || math::Vector3::Dot(m_normal,previous.m_normal)<0.95f) return;
        m_stickA=previous.m_stickA;
        m_stickB=previous.m_stickB;
        m_continuous=true;
    }

    void XPBDContact::ResetLambda()
    {
        m_penetration = 0.0f;
        m_reaction = math::Vector3::ZERO;
        m_lambdaNormal   = 0.0f;
        m_lambdaFriction = 0.0f;
        m_staticHeld = false;
        m_velocityNormal = 0.0f;
        m_velocityFriction = math::Vector3::ZERO;
        CaptureApproachSpeed();
    }

    void XPBDContact::CaptureApproachSpeed()
    {
        m_hasApproachSpeed = true;
        const auto pA=AnchorA();
        const auto pB=AnchorB();
        m_approachSpeed=math::Vector3::Dot(m_normal,
            PointVelocity(m_body,m_body ? pA-m_body->GetPosition() : math::Vector3::ZERO)
            -PointVelocity(m_other,m_other ? pB-m_other->GetPosition() : math::Vector3::ZERO));
    }

    void XPBDContact::SolvePosition(float h)
    {
        if (!m_body) return;

        const math::Vector3 pA = AnchorA();
        const math::Vector3 pB = AnchorB();
        const math::Vector3 rA = pA - m_body->GetPosition();
        const math::Vector3 rB = m_solveOther ? pB - m_other->GetPosition() : math::Vector3::ZERO;

        /// @note 生成時は pA == pB。法線方向へ離れたぶんだけ貫通が減る。
        const float penetration = m_depth - math::Vector3::Dot(m_normal, pA - pB);
        m_separation=-penetration;

        /// @note 反発の基準は位置補正前に一度だけ保存する。幾何回転後の腕長から衝突速度を再生成しない。
        if (!m_hasApproachSpeed) CaptureApproachSpeed();


        if (penetration <= 0.0f) {
            m_continuous = true;
            return;
        }
        m_penetration = std::max(m_penetration,penetration);
        if (m_other && !m_solveOther && !m_other->IsStatic()) m_other->WakeUp();

        RigidBody* solvedOther = m_solveOther ? m_other : nullptr;
        /// @note SolvePositional は correction の «逆» へ A を動かすので、法線の逆向きに渡す。
        SolvePositional(m_body, solvedOther, rA, rB,
                        m_normal * -penetration, 0.0f, h, m_lambdaNormal);

        /// @note 法線補正で姿勢が変わるため、摩擦の作用点も補正後の腕長で評価する。
        const math::Vector3 frictionA = AnchorA() - m_body->GetPosition();
        const math::Vector3 frictionB = m_solveOther
            ? AnchorB() - m_other->GetPosition() : math::Vector3::ZERO;
        SolveFriction(h, frictionA, frictionB);

        m_continuous = true;
    }

    /// @note 静摩擦。この substep で接線方向へ滑った距離をそのまま打ち消し、上限だけを
    /// @note クーロン (|λ_t| ≤ μ|λ_n|) に従わせる。速度パスで削る形にすると、倒れた体が
    /// @note 止まった後もじりじり滑る ── 削れるのは速度で、既にずれた位置は戻らないため。
    void XPBDContact::SolveFriction(float h, const math::Vector3& rA, const math::Vector3& rB)
    {
        (void)rA;
        (void)rB;
        /// @note 初回接触には過去の滑りを推定しない。連続接触の同一材質点だけを拘束する。
        /// @see https://matthias-research.github.io/pages/publications/PBDBodies.pdf §3.5 接線変位とクーロン上限。
        /// @note World の動的剛体へは対称な速度力積を返す。片側だけの位置固着を外部へ転送して射出しない。
        if ((m_other && !m_solveOther && !m_other->IsStatic())
            || !m_continuous || m_staticFriction<=0.0f || m_lambdaNormal==0.0f) return;
        const auto a=m_body->GetPosition()+m_body->GetRotation()*m_stickA;
        const auto b=m_other ? m_other->GetPosition()+m_other->GetRotation()*m_stickB : m_stickB;
        auto slip=a-b;
        slip-=m_normal*math::Vector3::Dot(m_normal,slip);
        const auto axis=slip.NormalizedOr(math::Vector3::RIGHT);
        const auto armA=a-m_body->GetPosition();
        const auto armB=m_solveOther ? b-m_other->GetPosition() : math::Vector3::ZERO;
        const float limit=m_staticFriction*std::abs(m_lambdaNormal);
        const float w=GeneralizedInverseMass(m_body,armA,axis)
            +GeneralizedInverseMass(m_solveOther ? m_other : nullptr,armB,axis);
        m_staticHeld=w>0.0f && slip.Length()/w<limit;
        if (m_staticHeld) {
            SolvePositional(m_body,m_solveOther ? m_other : nullptr,armA,armB,
                slip,0.0f,h,m_lambdaFriction,limit);
        } else {
            /// @note 滑り出したら次回の固着点を現在位置へ更新し、離れた古い点へ引き戻さない。
            const auto point=AnchorA();
            m_stickA=m_body->GetRotation().Inverse()*(point-m_body->GetPosition());
            m_stickB=m_other ? m_other->GetRotation().Inverse()*(point-m_other->GetPosition()) : point;
        }
    }

    void XPBDContact::SolveVelocity(float h)
    {
        if (!m_body || h <= 0.0f) return;

        const math::Vector3 pA = AnchorA();
        const math::Vector3 pB = AnchorB();
        const math::Vector3 rA = pA - m_body->GetPosition();
        RigidBody* solvedOther=m_solveOther ? m_other : nullptr;
        const bool externalDynamic=m_other && !m_solveOther && !m_other->IsStatic();
        const auto otherArm=m_other ? pB-m_other->GetPosition() : math::Vector3::ZERO;
        if (!m_hasApproachSpeed) CaptureApproachSpeed();
        const auto relativeVelocity=[&]() {
            auto otherVelocity=PointVelocity(m_other,otherArm);
            /// @note World の姿勢を再積分せず、この接触の未反映力積による速度だけを予測する。
            if (externalDynamic) otherVelocity+=m_reaction*m_other->GetInvMass()
                +math::Vector3::Cross(m_other->ApplyInvInertia(math::Vector3::Cross(otherArm,m_reaction)),otherArm);
            return PointVelocity(m_body,rA)-otherVelocity;
        };
        const auto applyImpulse=[&](const math::Vector3& impulse) {
            if (impulse.LengthSq()<=0.0f) return;
            m_body->ApplyImpulseAtPoint(impulse,pA);
            if (solvedOther) solvedOther->ApplyImpulseAtPoint(-impulse,pB);
            else if (m_other) m_reaction-=impulse;
        };
        const float normalSpeed=math::Vector3::Dot(m_normal,relativeVelocity());
        /// @note 最後の位置補正で開いた接触には反発や固着を残さない。
        const float separation = math::Vector3::Dot(m_normal, pA - pB) - m_depth;
        /// @note 接触許容を0にした場合も、位置解決の丸め誤差だけでは反発を捨てない。
        constexpr float contactRoundoff = 1.0e-6f;
        const bool touching = separation <= std::max(m_contactSlop, contactRoundoff);
        const float restitutionTarget=touching && m_restitution>0.0f && m_approachSpeed < -kRestitutionThreshold
            ? -m_restitution*m_approachSpeed : 0.0f;
        /// @note 離れた近接点は gap/h まで閉じてよく、空中で止めない。
        /// @see https://box2d.org/posts/2020/04/predictive-joint-limits/ speculative contact の速度下限。
        const float targetNormalSpeed=restitutionTarget-std::max(separation-m_contactSlop,0.0f)/h;
        /// @note 衝撃がある休止体を有効質量の計算前に起こし、軽い相手を無限質量の壁として解かない。
        if (normalSpeed<targetNormalSpeed) {
            if (!m_body->IsStatic()) m_body->WakeUp();
            if (m_other && !m_other->IsStatic()) m_other->WakeUp();
        }
        const float normalMass=GeneralizedInverseMass(m_body,rA,m_normal)
            +GeneralizedInverseMass(m_other,otherArm,m_normal);
        /// @note 累積した法線力積だけを非負へ制限し、反復中の負の差分による反力の撤回は許す。
        /// @see https://box2d.org/posts/2024/02/solver2d/ Projected Gauss-Seidel の accumulated impulse。
        const float previousNormal=m_velocityNormal;
        if (normalMass>0.0f) m_velocityNormal=std::max(0.0f,previousNormal+(targetNormalSpeed-normalSpeed)/normalMass);
        applyImpulse(m_normal*(m_velocityNormal-previousNormal));

        const auto relative=relativeVelocity();
        const auto tangential=relative-m_normal*math::Vector3::Dot(m_normal,relative);
        const float coefficient=touching ? (m_staticHeld ? m_staticFriction : m_friction) : 0.0f;
        const float budget=std::max(coefficient,0.0f)*m_velocityNormal;
        auto accumulated=m_velocityFriction;
        if (budget>0.0f && tangential.LengthSq()>0.0f) {
            const auto tangentX=math::Vector3::Cross(m_normal,std::abs(m_normal.y)<0.9f
                ? math::Vector3::UP : math::Vector3::RIGHT).Normalized();
            const auto tangentY=math::Vector3::Cross(m_normal,tangentX);
            const auto response=[&](const math::Vector3& impulse) {
                auto value=impulse*m_body->GetInvMass()
                    +math::Vector3::Cross(m_body->ApplyInvInertia(math::Vector3::Cross(rA,impulse)),rA);
                if (m_other) value+=impulse*m_other->GetInvMass()
                    +math::Vector3::Cross(m_other->ApplyInvInertia(math::Vector3::Cross(otherArm,impulse)),otherArm);
                return value;
            };
            const auto columnX=response(tangentX);
            const auto columnY=response(tangentY);
            const double a=math::Vector3::Dot(tangentX,columnX),b=math::Vector3::Dot(tangentX,columnY);
            const double c=math::Vector3::Dot(tangentY,columnX),d=math::Vector3::Dot(tangentY,columnY);
            const double determinant=a*d-b*c;
            const double x=math::Vector3::Dot(tangentX,tangential),y=math::Vector3::Dot(tangentY,tangential);
            /// @note 接線の2自由度を同時に解き、細長い部位の逆慣性で摩擦軸が相互に再加速する残差を減らす。
            if (determinant>1.0e-12) accumulated-=tangentX*static_cast<float>((d*x-b*y)/determinant)
                +tangentY*static_cast<float>((a*y-c*x)/determinant);
        }
        /// @note 法線の反力が減った反復では摩擦円錐も縮め、過去の摩擦力積を戻す。
        const float magnitude=accumulated.Length();
        if (magnitude>budget) accumulated=accumulated*(budget/magnitude);
        applyImpulse(accumulated-m_velocityFriction);
        m_velocityFriction=accumulated;
    }
} /// @note namespace fbzz::physics
