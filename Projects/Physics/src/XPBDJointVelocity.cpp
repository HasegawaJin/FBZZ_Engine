/// @file    XPBDJointVelocity.cpp
/// @brief   接続点と可動域に対する速度射影の準備と反復。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Physics/XPBDJoint.hpp>
#include "XPBDJointAngles.hpp"
#include <bit>
#include <cmath>
#include <cstdint>

namespace fbzz::physics
{
    namespace
    {
        constexpr int VELOCITY_PROJECTION_ITERATIONS = 8;
        constexpr float SOCKET_VELOCITY_TOLERANCE_SQ = 1.0e-12f;

        struct LimitJacobians
        {
            math::Vector3 twist;
            math::Vector3 swingY;
            math::Vector3 swingZ;
        };

        /// @note q̇=0.5[ω,0]q と ḋ=ω×d から、twist=2atan2(qx,qw)、swing=(0,-dz,dy)atan2(|dYZ|,dx)/|dYZ| を微分する。
        /// @note 180° swing は分解の特異点なので、未定義の速度勾配を使わず位置制限の解消を待つ。
        /// @see https://arxiv.org/abs/1711.02508 Quaternion kinematics for the error-state Kalman filter, quaternion derivative and angular velocity.
        LimitJacobians BuildLimitJacobians(const math::Quaternion& relativeRotation)
        {
            const double qx = relativeRotation.x;
            const double qy = relativeRotation.y;
            const double qz = relativeRotation.z;
            const double qw = relativeRotation.w;
            const double twistNormSquared = qx * qx + qw * qw;
            if (twistNormSquared <= 1.0e-8) return {};

            const auto childAxis = relativeRotation * math::Vector3::RIGHT;
            const double axisX = childAxis.x;
            const double axisY = childAxis.y;
            const double axisZ = childAxis.z;
            const double sineSquared = axisY * axisY + axisZ * axisZ;
            double swingScale = 1.0 + sineSquared / 6.0;
            double scaleDerivative = -1.0 / 3.0;
            if (sineSquared > 1.0e-8) {
                const double sine = std::sqrt(sineSquared);
                swingScale = std::atan2(sine, axisX) / sine;
                scaleDerivative = (swingScale * axisX - 1.0) / sineSquared;
            }
            return {
                {1.0f,
                 static_cast<float>((qw * qz + qx * qy) / twistNormSquared),
                 static_cast<float>((qx * qz - qw * qy) / twistNormSquared)},
                {static_cast<float>(-swingScale * axisY),
                 static_cast<float>(swingScale * axisX - scaleDerivative * axisZ * axisZ),
                 static_cast<float>(scaleDerivative * axisY * axisZ)},
                {static_cast<float>(-swingScale * axisZ),
                 static_cast<float>(scaleDerivative * axisY * axisZ),
                 static_cast<float>(swingScale * axisX - scaleDerivative * axisY * axisY)}};
        }

        struct SocketVelocitySystem
        {
            const math::Vector3& m_childOffset;
            const math::Vector3& m_parentOffset;
            const math::Vector3& m_columnX;
            const math::Vector3& m_columnY;
            const math::Vector3& m_columnZ;
            const math::Vector3& m_cofactorX;
            float m_determinant;

            math::Vector3 SolveImpulse(const math::Vector3& relativeAnchorVelocity) const
            {
                return {
                    math::Vector3::Dot(relativeAnchorVelocity, m_cofactorX) / m_determinant,
                    math::Vector3::Dot(m_columnX, math::Vector3::Cross(relativeAnchorVelocity, m_columnZ)) / m_determinant,
                    math::Vector3::Dot(m_columnX, math::Vector3::Cross(m_columnY, relativeAnchorVelocity)) / m_determinant};
            }
        };

        bool SameVelocityBits(const math::Vector3& first, const math::Vector3& second)
        {
            return std::bit_cast<std::uint32_t>(first.x) == std::bit_cast<std::uint32_t>(second.x)
                && std::bit_cast<std::uint32_t>(first.y) == std::bit_cast<std::uint32_t>(second.y)
                && std::bit_cast<std::uint32_t>(first.z) == std::bit_cast<std::uint32_t>(second.z);
        }

        void ProjectSocketInLocalState(const SocketVelocitySystem& socket, RigidBody& child, RigidBody& parent)
        {
            const float childInverseMass = child.GetInvMass();
            const float parentInverseMass = parent.GetInvMass();
            auto childLinearVelocity = child.GetVelocity();
            auto childAngularVelocity = child.GetAngularVelocity();
            auto parentLinearVelocity = parent.GetVelocity();
            auto parentAngularVelocity = parent.GetAngularVelocity();
            for (int iteration = 0; iteration < VELOCITY_PROJECTION_ITERATIONS; ++iteration) {
                const auto childAnchorVelocity = childLinearVelocity + math::Vector3::Cross(childAngularVelocity, socket.m_childOffset);
                const auto parentAnchorVelocity = parentLinearVelocity + math::Vector3::Cross(parentAngularVelocity, socket.m_parentOffset);
                const auto relativeAnchorVelocity = childAnchorVelocity - parentAnchorVelocity;
                /// @note ソケットの 3×3 解は通常 1 回で収束する。残差が 1 µm/s 以下なら力積の再計算を省く。
                if (relativeAnchorVelocity.LengthSq() <= SOCKET_VELOCITY_TOLERANCE_SQ) break;
                const auto impulse = socket.SolveImpulse(relativeAnchorVelocity);
                const auto nextChildLinearVelocity = childLinearVelocity - impulse * childInverseMass;
                const auto nextChildAngularVelocity = childAngularVelocity - child.ApplyInvInertia(math::Vector3::Cross(socket.m_childOffset, impulse));
                const auto nextParentLinearVelocity = parentLinearVelocity + impulse * parentInverseMass;
                const auto nextParentAngularVelocity = parentAngularVelocity + parent.ApplyInvInertia(math::Vector3::Cross(socket.m_parentOffset, impulse));
                if (SameVelocityBits(nextChildLinearVelocity, childLinearVelocity) && SameVelocityBits(nextChildAngularVelocity, childAngularVelocity)
                    && SameVelocityBits(nextParentLinearVelocity, parentLinearVelocity) && SameVelocityBits(nextParentAngularVelocity, parentAngularVelocity))
                    break;
                childLinearVelocity = nextChildLinearVelocity;
                childAngularVelocity = nextChildAngularVelocity;
                parentLinearVelocity = nextParentLinearVelocity;
                parentAngularVelocity = nextParentAngularVelocity;
            }
            child.SetVelocity(childLinearVelocity);
            child.SetAngularVelocity(childAngularVelocity);
            parent.SetVelocity(parentLinearVelocity);
            parent.SetAngularVelocity(parentAngularVelocity);
        }
    } /// @note namespace

    XPBDJoint::VelocityProjection XPBDJoint::PrepareVelocityProjection() const
    {
        VelocityProjection projection;
        if (!m_child) return projection;
        /// @note 位置射影後の速度も関節の許容運動へ射影する。脱力中も接続点の速度を一致させ、自由回転は残す。
        /// @see https://matthias-research.github.io/pages/publications/PBDBodies.pdf Velocity Update.
        if (m_socketCompliance <= 0.0f) {
            projection.m_childArm = m_child->GetRotation() * m_anchorChild;
            projection.m_parentArm = m_parent ? m_parent->GetRotation() * m_anchorParent : math::Vector3::ZERO;
            const auto anchorVelocityResponse = [&](const math::Vector3& impulse) {
                const auto& childArm = projection.m_childArm;
                const auto& parentArm = projection.m_parentArm;
                auto velocityChange = impulse * (m_child->GetInvMass() + (m_parent ? m_parent->GetInvMass() : 0.0f));
                velocityChange += math::Vector3::Cross(m_child->ApplyInvInertia(math::Vector3::Cross(childArm, impulse)), childArm);
                if (m_parent)
                    velocityChange += math::Vector3::Cross(m_parent->ApplyInvInertia(math::Vector3::Cross(parentArm, impulse)), parentArm);
                return velocityChange;
            };
            projection.m_columnX = anchorVelocityResponse(math::Vector3::RIGHT);
            projection.m_columnY = anchorVelocityResponse(math::Vector3::UP);
            projection.m_columnZ = anchorVelocityResponse(math::Vector3::FORWARD);
            projection.m_cofactorX = math::Vector3::Cross(projection.m_columnY, projection.m_columnZ);
            projection.m_determinant = math::Vector3::Dot(projection.m_columnX, projection.m_cofactorX);
        }
        if (m_limits.enabled && m_limits.compliance <= 0.0f) {
            const auto parentFrame = ParentFrameWorld();
            const auto relativeRotation = (parentFrame.Inverse() * ChildFrameWorld()).Normalized();
            const auto angles = joint_detail::MeasureAngles(relativeRotation);
            constexpr float LIMIT_ANGLE_TOLERANCE = 0.0001f;
            const auto atLimit = [](float angle, float low, float high) {
                return angle <= low + LIMIT_ANGLE_TOLERANCE || angle >= high - LIMIT_ANGLE_TOLERANCE;
            };
            const bool atSwingYLimit = atLimit(angles.swingRotationVector.y, m_limits.swingMinY, m_limits.swingMaxY);
            const bool atSwingZLimit = atLimit(angles.swingRotationVector.z, m_limits.swingMinZ, m_limits.swingMaxZ);
            const bool atTwistLimit = atLimit(angles.twistAngle, m_limits.twistMin, m_limits.twistMax);
            if (atSwingYLimit || atSwingZLimit || atTwistLimit) {
                /// @note 速度射影の反復では姿勢が変わらない。上下限に達した軸を一度だけ用意する。
                const auto jacobians = BuildLimitJacobians(relativeRotation);
                const auto addLimit = [&](const math::Vector3& gradient, float angle, float low, float high) {
                    projection.m_limits[projection.m_limitCount++] = {
                        (parentFrame * gradient).NormalizedOr(math::Vector3::ZERO),
                        angle <= low + LIMIT_ANGLE_TOLERANCE,
                        angle >= high - LIMIT_ANGLE_TOLERANCE,
                    };
                };
                if (atSwingYLimit) addLimit(jacobians.swingY, angles.swingRotationVector.y, m_limits.swingMinY, m_limits.swingMaxY);
                if (atSwingZLimit) addLimit(jacobians.swingZ, angles.swingRotationVector.z, m_limits.swingMinZ, m_limits.swingMaxZ);
                if (atTwistLimit) addLimit(jacobians.twist, angles.twistAngle, m_limits.twistMin, m_limits.twistMax);
            }
        }
        return projection;
    }

    void XPBDJoint::ProjectVelocity(const VelocityProjection& projection)
    {
        const bool projectSocket = projection.m_determinant > 1.0e-9f;
        const bool socketOnly = projection.m_limitCount == 0;
        if (!projectSocket && socketOnly) return;

        const SocketVelocitySystem socket{
            projection.m_childArm, projection.m_parentArm,
            projection.m_columnX, projection.m_columnY, projection.m_columnZ,
            projection.m_cofactorX, projection.m_determinant};
        const auto canDeferVelocityWrites = [](const RigidBody& body) {
            const auto positionLock = body.GetFreezePosition();
            const auto rotationLock = body.GetFreezeRotation();
            return !body.m_allowSleeping && !body.IsSleeping()
                && std::bit_cast<std::uint32_t>(body.m_sleepTimer) == 0
                && !positionLock.x && !positionLock.y && !positionLock.z
                && !rotationLock.x && !rotationLock.y && !rotationLock.z;
        };
        /// @note 休止と軸固定の影響がない二剛体間では、関節内の反復結果だけを先に求めて一度書き戻す。
        if (socketOnly && m_parent && m_parent != m_child
            && canDeferVelocityWrites(*m_child) && canDeferVelocityWrites(*m_parent)) {
            ProjectSocketInLocalState(socket, *m_child, *m_parent);
            return;
        }
        /// @note 可動域とソケットの速度射影を交互に解く。ソケットだけで角速度を再生成して可動域へ突っ込ませない。
        for (int iteration = 0; iteration < VELOCITY_PROJECTION_ITERATIONS; ++iteration) {
            bool adjustedLimit = false;
            for (int index = 0; index < projection.m_limitCount; ++index) {
                const auto& limit = projection.m_limits[index];
                const auto relativeAngularVelocity = m_child->GetAngularVelocity() -
                    (m_parent ? m_parent->GetAngularVelocity() : math::Vector3::ZERO);
                const float limitAxisSpeed = math::Vector3::Dot(relativeAngularVelocity, limit.axis);
                if ((limit.atUpper && limitAxisSpeed > 0.0f) || (limit.atLower && limitAxisSpeed < 0.0f)) {
                    ApplyAngularVelocityChange(m_child, m_parent, limit.axis * limitAxisSpeed);
                    adjustedLimit = true;
                }
            }
            if (projectSocket) {
                /// @note 3軸を別々に解くと細い肢の大きな逆慣性で収束が遅い。点の3×3有効質量を同時に解く。
                const auto oldChildLinear = m_child->GetVelocity();
                const auto oldChildAngular = m_child->GetAngularVelocity();
                const auto oldParentLinear = m_parent ? m_parent->GetVelocity() : math::Vector3::ZERO;
                const auto oldParentAngular = m_parent ? m_parent->GetAngularVelocity() : math::Vector3::ZERO;
                const bool childSleeping = m_child->IsSleeping();
                const bool parentSleeping = m_parent && m_parent->IsSleeping();
                const auto childAnchorVelocity = oldChildLinear + math::Vector3::Cross(oldChildAngular, socket.m_childOffset);
                const auto parentAnchorVelocity = m_parent ? oldParentLinear + math::Vector3::Cross(oldParentAngular, socket.m_parentOffset) : math::Vector3::ZERO;
                const auto relativeAnchorVelocity = childAnchorVelocity - parentAnchorVelocity;
                if (relativeAnchorVelocity.LengthSq() <= SOCKET_VELOCITY_TOLERANCE_SQ) {
                    if (!adjustedLimit) break;
                    continue;
                }
                const auto impulse = socket.SolveImpulse(relativeAnchorVelocity);
                m_child->SetVelocity(m_child->GetVelocity()-impulse*m_child->GetInvMass());
                m_child->SetAngularVelocity(m_child->GetAngularVelocity()-m_child->ApplyInvInertia(math::Vector3::Cross(socket.m_childOffset,impulse)));
                if (m_parent) {
                    m_parent->SetVelocity(m_parent->GetVelocity()+impulse*m_parent->GetInvMass());
                    m_parent->SetAngularVelocity(m_parent->GetAngularVelocity()+m_parent->ApplyInvInertia(math::Vector3::Cross(socket.m_parentOffset,impulse)));
                }
                /// @note ソケットだけを反復するとき、両者の速度と休止状態が同じなら次回も同じ結果になる。
                if (socketOnly && m_child->IsSleeping() == childSleeping
                    && SameVelocityBits(m_child->GetVelocity(), oldChildLinear)
                    && SameVelocityBits(m_child->GetAngularVelocity(), oldChildAngular)
                    && (!m_parent || (m_parent->IsSleeping() == parentSleeping
                        && SameVelocityBits(m_parent->GetVelocity(), oldParentLinear)
                        && SameVelocityBits(m_parent->GetAngularVelocity(), oldParentAngular))))
                    break;
            } else if (!adjustedLimit) break;
        }
    }
} /// @note namespace fbzz::physics
