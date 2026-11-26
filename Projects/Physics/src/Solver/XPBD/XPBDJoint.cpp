/// @file    XPBDJoint.cpp
/// @brief   関節フレームとアンカーの構築、位置拘束を解く順序。
/// @author  Hasegawa Jin
/// @date    2026-09-02

#include <Physics/Solver/XPBD/XPBDJoint.hpp>

namespace fbzz::physics
{
    XPBDJoint::XPBDJoint(RigidBody* parent, RigidBody* child)
        : m_parent(parent)
        , m_child(child)
    {
    }

    void XPBDJoint::Build(const math::Vector3& worldAnchor, const math::Quaternion& worldFrame)
    {
        const math::Quaternion normalizedWorldFrame = worldFrame.Normalized();

        if (m_parent) {
            const math::Quaternion inverseBodyRotation = m_parent->GetRotation().Inverse();
            m_anchorParent = inverseBodyRotation * (worldAnchor - m_parent->GetPosition());
            m_frameParent  = (inverseBodyRotation * normalizedWorldFrame).Normalized();
        } else {
            // 親が居ない関節はワールドへ固定する。ローカル量をそのままワールド量として持つ。
            m_anchorParent = worldAnchor;
            m_frameParent  = normalizedWorldFrame;
        }

        if (m_child) {
            const math::Quaternion inverseBodyRotation = m_child->GetRotation().Inverse();
            m_anchorChild = inverseBodyRotation * (worldAnchor - m_child->GetPosition());
            m_frameChild  = (inverseBodyRotation * normalizedWorldFrame).Normalized();
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
        m_driveImpulse = math::Vector3::ZERO;
    }

    void XPBDJoint::SolvePosition(float substepDuration)
    {
        if (!m_child) {
            return;
        }

        // Gauss-Seidel は «後に解いたものが勝つ»。ドライブ (柔らかい目標) を先に、可動域
        // (硬い壁) を次に、ソケット (伸びてはいけない) を最後に置く。逆順だと力み切った
        // ドライブが関節を可動域の外へ押し出したまま残る。
        SolveDrive(substepDuration);
        SolveLimits(substepDuration);
        SolveSocket(substepDuration);
    }

    void XPBDJoint::SolvePhysicalPosition(float substepDuration)
    {
        if (!m_child) {
            return;
        }
        SolveDrive(substepDuration);
        if (m_limits.compliance > 0.0f) {
            SolveLimits(substepDuration);
        }
        if (m_socketCompliance > 0.0f) {
            SolveSocket(substepDuration);
        }
    }

    void XPBDJoint::SolveCorrectionPosition(float substepDuration)
    {
        if (!m_child) {
            return;
        }
        if (m_limits.compliance <= 0.0f) {
            SolveLimits(substepDuration);
        }
        if (m_socketCompliance <= 0.0f) {
            SolveSocket(substepDuration);
        }
    }

    void XPBDJoint::SolveSocket(float substepDuration)
    {
        const math::Vector3 childAnchorOffset = m_child->GetRotation() * m_anchorChild;
        const math::Vector3 childAnchorPosition = m_child->GetPosition() + childAnchorOffset;
        const math::Vector3 parentAnchorOffset =
            m_parent ? m_parent->GetRotation() * m_anchorParent : math::Vector3::ZERO;
        const math::Vector3 parentAnchorPosition = m_parent ? m_parent->GetPosition() + parentAnchorOffset : m_anchorParent;

        SolvePositional(m_child, m_parent, childAnchorOffset, parentAnchorOffset,
                        childAnchorPosition - parentAnchorPosition, m_socketCompliance, substepDuration, m_lambdaSocket);
    }

    void XPBDJoint::SolveVelocity(float substepDuration)
    {
        if (!m_child || substepDuration <= 0.0f) {
            return;
        }
        SolveDriveVelocity(substepDuration);
        ProjectVelocity(PrepareVelocityProjection());
    }
} /// @note namespace fbzz::physics
