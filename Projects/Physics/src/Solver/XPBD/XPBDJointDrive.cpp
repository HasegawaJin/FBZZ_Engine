/// @file    XPBDJointDrive.cpp
/// @brief   目標姿勢への追従と相対角速度の減衰を同じトルク予算で解く。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/Solver/XPBD/XPBDJoint.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::physics
{
    void XPBDJoint::SolveDrive(float substepDuration)
    {
        m_driveSaturated = false;
        if (!m_drive.enabled || substepDuration <= 0.0f) return;

        const math::Quaternion parentFrame = ParentFrameWorld();
        const math::Quaternion childFrame = ChildFrameWorld();
        const math::Quaternion targetWorldFrame = (parentFrame * m_drive.target).Normalized();

        /// @note 目標フレームから «今どれだけ回っているか»。子はこの逆へ回れば目標に着く。
        const math::Vector3 rotationError = RotationVector((childFrame * targetWorldFrame.Inverse()).Normalized());

        const float maxDriveLambda = m_drive.maxTorque > 0.0f
            ? m_drive.maxTorque * substepDuration * substepDuration : 0.0f;
        const float driveLambdaChange = SolveAngular(m_child, m_parent, rotationError,
            m_drive.compliance, substepDuration, m_lambdaDrive, maxDriveLambda);
        m_driveImpulse += rotationError.NormalizedOr(math::Vector3::ZERO) * (driveLambdaChange / substepDuration);

        /// @note λ が上限に張り付いた ＝ 出したいトルクを出せていない ＝ 力負けしている。
        m_driveSaturated =
            maxDriveLambda > 0.0f && std::abs(m_lambdaDrive) >= maxDriveLambda * 0.9999f;
    }

    void XPBDJoint::SolveDriveVelocity(float substepDuration)
    {
        if (!m_drive.enabled || m_drive.damping <= 0.0f) return;

        const math::Vector3 parentAngularVelocity =
            m_parent ? m_parent->GetAngularVelocity() : math::Vector3::ZERO;
        const math::Vector3 relativeAngularVelocity = m_child->GetAngularVelocity() - parentAngularVelocity;

        /// @note 1 ステップで消せるのは «全部» まで。damping·h が 1 を超えると符号が反転して発散する。
        const float dampingFraction = std::min(m_drive.damping * substepDuration, 1.0f);
        const auto dampingAxis = relativeAngularVelocity.NormalizedOr(math::Vector3::ZERO);
        const float angularInverseMass = math::Vector3::Dot(dampingAxis, m_child->ApplyInvInertia(dampingAxis))
            + (m_parent ? math::Vector3::Dot(dampingAxis, m_parent->ApplyInvInertia(dampingAxis)) : 0.0f);
        if (angularInverseMass <= 1.0e-9f) return;
        auto combinedDriveImpulse = m_driveImpulse - relativeAngularVelocity * (dampingFraction / angularInverseMass);
        /// @note 位置パスの λ/h と減衰を同じ角力積へ合算し、逆向きの減衰は既存の出力を撤回できる。
        /// @see https://box2d.org/posts/2024/02/solver2d/ Projected Gauss-Seidel の accumulated impulse。
        const float maxDriveImpulse = m_drive.maxTorque * substepDuration;
        const float requestedImpulseMagnitude = combinedDriveImpulse.Length();
        if (maxDriveImpulse > 0.0f && requestedImpulseMagnitude > maxDriveImpulse)
            combinedDriveImpulse = combinedDriveImpulse * (maxDriveImpulse / requestedImpulseMagnitude);
        const auto dampingImpulse = combinedDriveImpulse - m_driveImpulse;
        m_child->SetAngularVelocity(m_child->GetAngularVelocity() + m_child->ApplyInvInertia(dampingImpulse));
        if (m_parent)
            m_parent->SetAngularVelocity(m_parent->GetAngularVelocity() - m_parent->ApplyInvInertia(dampingImpulse));
        m_driveImpulse = combinedDriveImpulse;
        /// @note 位置追従が既に出力不足なら、逆向きの減衰で合計出力が減ってもその substep の飽和診断を残す。
        m_driveSaturated = m_driveSaturated
            || (maxDriveImpulse > 0.0f && requestedImpulseMagnitude >= maxDriveImpulse * 0.9999f);
    }
} /// @note namespace fbzz::physics
