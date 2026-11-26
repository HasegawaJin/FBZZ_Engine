/// @file    XPBDJointLimits.cpp
/// @brief   可動域の計測、位置補正、現在姿勢からの学習。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/Solver/XPBD/XPBDJoint.hpp>
#include <Math/Scalar/MathUtils.hpp>
#include "XPBDJointAngles.hpp"
#include <algorithm>
#include <cmath>

namespace fbzz::physics
{
    namespace
    {
        constexpr float MIN_ANGLE_CORRECTION = 1.0e-6f;
        /// @note 診断で «可動域に当たっている» と報告する食い込み量 [rad] ≒ 0.6°。
        /// @note 位置補正と同じ閾値で報告すると数値誤差でほぼ常に true になる。
        constexpr float LIMIT_REPORT_ANGLE = 0.01f;

        /// @note twist から符号付きの角度 [rad] を取り出す。X 軸まわりのみ。
        float TwistAngle(const math::Quaternion& twist)
        {
            float axisComponent = twist.x;
            float scalarComponent = twist.w;
            // 短い方の弧
            if (scalarComponent < 0.0f) {
                axisComponent = -axisComponent;
                scalarComponent = -scalarComponent;
            }
            return 2.0f * std::atan2(axisComponent, scalarComponent);
        }
    }

    joint_detail::JointAngles joint_detail::MeasureAngles(const math::Quaternion& relativeRotation)
    {
        math::Quaternion swing;
        math::Quaternion twist;
        DecomposeSwingTwist(relativeRotation, swing, twist);
        return {swing, RotationVector(swing), TwistAngle(twist)};
    }

    void XPBDJoint::SolveLimits(float substepDuration)
    {
        // 診断値はドライブの後・ソケットの前で測る。可動域に対してどこに居るかを見たいので、
        // «押し戻す前» の値を残す。
        const math::Quaternion parentFrame = ParentFrameWorld();
        const math::Quaternion childFrame = ChildFrameWorld();
        const math::Quaternion relativeRotation = (parentFrame.Inverse() * childFrame).Normalized();

        const auto angles = joint_detail::MeasureAngles(relativeRotation);
        m_twistAngle  = angles.twistAngle;
        m_swingAngleY = angles.swingRotationVector.y;
        m_swingAngleZ = angles.swingRotationVector.z;

        m_limited = false;
        if (!m_limits.enabled) {
            return;
        }

        // スイング: 軸ごとの超過分をまとめて 1 回の角度補正にする。厳密には Y と Z の
        // 回転は交換しないが、超過は小さい前提なので合成で足りる。
        const float swingExcessY =
            m_swingAngleY - math::Clamp(m_swingAngleY, m_limits.swingMinY, m_limits.swingMaxY);
        const float swingExcessZ =
            m_swingAngleZ - math::Clamp(m_swingAngleZ, m_limits.swingMinZ, m_limits.swingMaxZ);

        if (std::abs(swingExcessY) > MIN_ANGLE_CORRECTION || std::abs(swingExcessZ) > MIN_ANGLE_CORRECTION) {
            const math::Vector3 correction = parentFrame * math::Vector3{ 0.0f, swingExcessY, swingExcessZ };
            SolveAngular(m_child, m_parent, correction, m_limits.compliance, substepDuration, m_lambdaSwing);
            m_limited = m_limited ||
                        std::abs(swingExcessY) > LIMIT_REPORT_ANGLE || std::abs(swingExcessZ) > LIMIT_REPORT_ANGLE;
        }

        // ツイスト: スイングで傾いた «後» の骨の軸まわりに戻す。親フレームの X で回すと、
        // 大きく振れているときに戻す向きがずれてスイングを増やしてしまう。
        const float twistExcess =
            m_twistAngle - math::Clamp(m_twistAngle, m_limits.twistMin, m_limits.twistMax);
        if (std::abs(twistExcess) > MIN_ANGLE_CORRECTION) {
            const math::Vector3 axis =
                (parentFrame * (angles.swingRotation * math::Vector3::RIGHT)).NormalizedOr(math::Vector3::RIGHT);
            SolveAngular(m_child, m_parent, axis * twistExcess,
                         m_limits.compliance, substepDuration, m_lambdaTwist);
            m_limited = m_limited || std::abs(twistExcess) > LIMIT_REPORT_ANGLE;
        }
    }

    void XPBDJoint::LearnLimits(const math::Quaternion& relativeRotation, float margin)
    {
        if (!m_limits.enabled) {
            return;
        }

        const float angleMargin = std::max(margin, 0.0f);

        const auto angles = joint_detail::MeasureAngles(relativeRotation.Normalized());

        m_limits.twistMin  = std::min(m_limits.twistMin,  angles.twistAngle - angleMargin);
        m_limits.twistMax  = std::max(m_limits.twistMax,  angles.twistAngle + angleMargin);
        m_limits.swingMinY = std::min(m_limits.swingMinY, angles.swingRotationVector.y - angleMargin);
        m_limits.swingMaxY = std::max(m_limits.swingMaxY, angles.swingRotationVector.y + angleMargin);
        m_limits.swingMinZ = std::min(m_limits.swingMinZ, angles.swingRotationVector.z - angleMargin);
        m_limits.swingMaxZ = std::max(m_limits.swingMaxZ, angles.swingRotationVector.z + angleMargin);
    }

    void XPBDJoint::LearnLimitsFromCurrentPose(float margin)
    {
        LearnLimits((ParentFrameWorld().Inverse() * ChildFrameWorld()).Normalized(), margin);
    }
} /// @note namespace fbzz::physics
