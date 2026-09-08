/// @file    XPBDJoint.cpp
/// @brief   ボールソケット + swing/twist 制限 + 角度ドライブ
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <Physics/XPBDJoint.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::physics
{
    namespace
    {
        constexpr float kMinAngle = 1.0e-6f;
        // 診断で «可動域に当たっている» と報告する食い込み量 [rad] ≒ 0.6°。
        // kMinAngle で報告すると数値誤差でほぼ常に true になり、切り分けに使えない。
        constexpr float kReportAngle = 0.01f;

        float ClampToRange(float value, float low, float high)
        {
            return value < low ? low : (value > high ? high : value);
        }

        /// twist から符号付きの角度 [rad] を取り出す。X 軸まわりのみ。
        float TwistAngle(const math::Quaternion& twist)
        {
            float x = twist.x;
            float w = twist.w;
            if (w < 0.0f) { x = -x; w = -w; }  // 短い方の弧
            return 2.0f * std::atan2(x, w);
        }
    } // namespace

    XPBDJoint::XPBDJoint(RigidBody* parent, RigidBody* child)
        : m_parent(parent)
        , m_child(child)
    {
    }

    void XPBDJoint::Build(const math::Vector3& worldAnchor, const math::Quaternion& worldFrame)
    {
        const math::Quaternion frame = worldFrame.Normalized();

        if (m_parent) {
            const math::Quaternion inverse = m_parent->GetRotation().Inverse();
            m_anchorParent = inverse * (worldAnchor - m_parent->GetPosition());
            m_frameParent  = (inverse * frame).Normalized();
        } else {
            // 親が居ない関節はワールドへ固定する。ローカル量をそのままワールド量として持つ。
            m_anchorParent = worldAnchor;
            m_frameParent  = frame;
        }

        if (m_child) {
            const math::Quaternion inverse = m_child->GetRotation().Inverse();
            m_anchorChild = inverse * (worldAnchor - m_child->GetPosition());
            m_frameChild  = (inverse * frame).Normalized();
        }
    }

    math::Quaternion XPBDJoint::ParentFrameWorld() const
    {
        return m_parent ? (m_parent->GetRotation() * m_frameParent).Normalized() : m_frameParent;
    }

    math::Quaternion XPBDJoint::ChildFrameWorld() const
    {
        return m_child ? (m_child->GetRotation() * m_frameChild).Normalized() : m_frameChild;
    }

    math::Vector3 XPBDJoint::ParentAnchorWorld() const
    {
        return m_parent ? m_parent->GetPosition() + m_parent->GetRotation() * m_anchorParent
                        : m_anchorParent;
    }

    void XPBDJoint::ResetLambda()
    {
        m_lambdaSocket = 0.0f;
        m_lambdaTwist  = 0.0f;
        m_lambdaSwing  = 0.0f;
        m_lambdaDrive  = 0.0f;
    }

    void XPBDJoint::SolvePosition(float h)
    {
        if (!m_child) return;

        // WHY この順か: Gauss-Seidel は «後に解いたものが勝つ»。ドライブ (柔らかい目標) を
        //     先に、可動域 (硬い壁) を次に、ソケット (伸びてはいけない) を最後に置く。
        //     逆順だと、力み切ったドライブが関節を可動域の外へ押し出したまま残る。
        SolveDrive(h);
        SolveLimits(h);
        SolveSocket(h);
    }

    void XPBDJoint::SolveDrive(float h)
    {
        m_driveSaturated = false;
        if (!m_drive.enabled) return;

        const math::Quaternion parentFrame = ParentFrameWorld();
        const math::Quaternion childFrame  = ChildFrameWorld();
        const math::Quaternion goal        = (parentFrame * m_drive.target).Normalized();

        // 目標フレームから «今どれだけ回っているか»。子はこの逆へ回れば目標に着く。
        const math::Vector3 correction = RotationVector((childFrame * goal.Inverse()).Normalized());

        const float maxLambda = m_drive.maxTorque > 0.0f ? m_drive.maxTorque * h * h : 0.0f;
        SolveAngular(m_child, m_parent, correction, m_drive.compliance, h, m_lambdaDrive, maxLambda);

        // λ が上限に張り付いた ＝ 出したいトルクを出せていない ＝ 力負けしている。
        m_driveSaturated =
            maxLambda > 0.0f && std::abs(m_lambdaDrive) >= maxLambda * 0.9999f;
    }

    void XPBDJoint::SolveLimits(float h)
    {
        // 診断値はドライブの後・ソケットの前で測る。可動域に対してどこに居るかを見たいので、
        // «押し戻す前» の値を残す。
        const math::Quaternion parentFrame = ParentFrameWorld();
        const math::Quaternion childFrame  = ChildFrameWorld();
        const math::Quaternion deviation   = (parentFrame.Inverse() * childFrame).Normalized();

        math::Quaternion swing;
        math::Quaternion twist;
        DecomposeSwingTwist(deviation, swing, twist);

        const math::Vector3 swingVector = RotationVector(swing);
        m_twistAngle  = TwistAngle(twist);
        m_swingAngleY = swingVector.y;
        m_swingAngleZ = swingVector.z;

        m_limited = false;
        if (!m_limits.enabled) return;

        // スイング: 軸ごとの超過分をまとめて 1 回の角度補正にする。厳密には Y と Z の
        // 回転は交換しないが、超過は小さい前提なので合成で足りる。
        const float excessY =
            m_swingAngleY - ClampToRange(m_swingAngleY, m_limits.swingMinY, m_limits.swingMaxY);
        const float excessZ =
            m_swingAngleZ - ClampToRange(m_swingAngleZ, m_limits.swingMinZ, m_limits.swingMaxZ);

        if (std::abs(excessY) > kMinAngle || std::abs(excessZ) > kMinAngle) {
            const math::Vector3 correction = parentFrame * math::Vector3{ 0.0f, excessY, excessZ };
            SolveAngular(m_child, m_parent, correction, m_limits.compliance, h, m_lambdaSwing);
            m_limited = m_limited ||
                        std::abs(excessY) > kReportAngle || std::abs(excessZ) > kReportAngle;
        }

        // ツイスト: スイングで傾いた «後» の骨の軸まわりに戻す。親フレームの X で回すと、
        // 大きく振れているときに戻す向きがずれてスイングを増やしてしまう。
        const float excessTwist =
            m_twistAngle - ClampToRange(m_twistAngle, m_limits.twistMin, m_limits.twistMax);
        if (std::abs(excessTwist) > kMinAngle) {
            const math::Vector3 axis =
                (parentFrame * (swing * math::Vector3::RIGHT)).NormalizedOr(math::Vector3::RIGHT);
            SolveAngular(m_child, m_parent, axis * excessTwist,
                         m_limits.compliance, h, m_lambdaTwist);
            m_limited = m_limited || std::abs(excessTwist) > kReportAngle;
        }
    }

    void XPBDJoint::LearnLimits(const math::Quaternion& deviation, float margin)
    {
        if (!m_limits.enabled) return;

        const float safeMargin = std::max(margin, 0.0f);

        math::Quaternion swing;
        math::Quaternion twist;
        DecomposeSwingTwist(deviation.Normalized(), swing, twist);

        // SolveLimits が押し戻しの判定に使うのと同じ 3 つの角。
        const math::Vector3 swingVector = RotationVector(swing);
        const float         twistAngle  = TwistAngle(twist);

        m_limits.twistMin  = std::min(m_limits.twistMin,  twistAngle - safeMargin);
        m_limits.twistMax  = std::max(m_limits.twistMax,  twistAngle + safeMargin);
        m_limits.swingMinY = std::min(m_limits.swingMinY, swingVector.y - safeMargin);
        m_limits.swingMaxY = std::max(m_limits.swingMaxY, swingVector.y + safeMargin);
        m_limits.swingMinZ = std::min(m_limits.swingMinZ, swingVector.z - safeMargin);
        m_limits.swingMaxZ = std::max(m_limits.swingMaxZ, swingVector.z + safeMargin);
    }

    void XPBDJoint::LearnLimitsFromCurrentPose(float margin)
    {
        LearnLimits((ParentFrameWorld().Inverse() * ChildFrameWorld()).Normalized(), margin);
    }

    void XPBDJoint::SolveSocket(float h)
    {
        const math::Vector3 rChild = m_child->GetRotation() * m_anchorChild;
        const math::Vector3 pChild = m_child->GetPosition() + rChild;
        const math::Vector3 rParent =
            m_parent ? m_parent->GetRotation() * m_anchorParent : math::Vector3::ZERO;
        const math::Vector3 pParent = ParentAnchorWorld();

        SolvePositional(m_child, m_parent, rChild, rParent,
                        pChild - pParent, m_socketCompliance, h, m_lambdaSocket);
    }

    void XPBDJoint::SolveVelocity(float h)
    {
        if (!m_child || !m_drive.enabled || m_drive.damping <= 0.0f || h <= 0.0f) return;

        const math::Vector3 parentOmega =
            m_parent ? m_parent->GetAngularVelocity() : math::Vector3::ZERO;
        const math::Vector3 relative = m_child->GetAngularVelocity() - parentOmega;

        // 1 ステップで消せるのは «全部» まで。damping·h が 1 を超えると符号が反転して発散する。
        const float factor = std::min(m_drive.damping * h, 1.0f);
        // 減衰も関節が出す力なので、位置パスと同じトルク上限に従わせる。従わせないと
        // «2 N·m しか出せない関節が、減衰では 7 N·m 出して荷重を支える» ことになり、
        // 力負けしているはずの関節がゆっくり降りるだけになる。
        const float maxImpulse = m_drive.maxTorque > 0.0f ? m_drive.maxTorque * h : 0.0f;
        ApplyAngularVelocityChange(m_child, m_parent, relative * factor, maxImpulse);
    }
} // namespace fbzz::physics
