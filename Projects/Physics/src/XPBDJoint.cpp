/// @file    XPBDJoint.cpp
/// @brief   ボールソケット + swing/twist 制限 + 角度ドライブ
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <Physics/XPBDJoint.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace fbzz::physics
{
    namespace
    {
        constexpr float kMinAngle = 1.0e-6f;
        /// @note 診断で «可動域に当たっている» と報告する食い込み量 [rad] ≒ 0.6°。
        /// @note kMinAngle で報告すると数値誤差でほぼ常に true になり、切り分けに使えない。
        constexpr float kReportAngle = 0.01f;

        float ClampToRange(float value, float low, float high)
        {
            return value < low ? low : (value > high ? high : value);
        }

        /// @note twist から符号付きの角度 [rad] を取り出す。X 軸まわりのみ。
        float TwistAngle(const math::Quaternion& twist)
        {
            float x = twist.x;
            float w = twist.w;
            /// @note 短い方の弧
            if (w < 0.0f) { x = -x; w = -w; }
            return 2.0f * std::atan2(x, w);
        }

        struct LimitJacobians
        {
            math::Vector3 twist;
            math::Vector3 swingY;
            math::Vector3 swingZ;
        };

        /// @note q̇=0.5[ω,0]q と ḋ=ω×d から、twist=2atan2(qx,qw)、swing=(0,-dz,dy)atan2(|dYZ|,dx)/|dYZ| を微分する。
        /// @note 180° swing は分解の特異点なので、未定義の速度勾配を使わず位置制限の解消を待つ。
        /// @see https://arxiv.org/abs/1711.02508 Quaternion kinematics for the error-state Kalman filter, quaternion derivative and angular velocity.
        LimitJacobians BuildLimitJacobians(const math::Quaternion& rotation)
        {
            const double x=rotation.x,y=rotation.y,z=rotation.z,w=rotation.w;
            const double denominator=x*x+w*w;
            if (denominator<=1.0e-8) return {};
            const auto direction=rotation*math::Vector3::RIGHT;
            const double c=direction.x,a=direction.y,b=direction.z;
            const double sineSquared=a*a+b*b;
            double factor=1.0+sineSquared/6.0;
            double derivative=-1.0/3.0;
            if (sineSquared>1.0e-8) {
                const double sine=std::sqrt(sineSquared);
                factor=std::atan2(sine,c)/sine;
                derivative=(factor*c-1.0)/sineSquared;
            }
            return {
                {1.0f,static_cast<float>((w*z+x*y)/denominator),static_cast<float>((x*z-w*y)/denominator)},
                {static_cast<float>(-factor*a),static_cast<float>(factor*c-derivative*b*b),static_cast<float>(derivative*a*b)},
                {static_cast<float>(-factor*b),static_cast<float>(derivative*a*b),static_cast<float>(factor*c-derivative*a*a)}};
        }
    } /// @note namespace

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
            /// @note 親が居ない関節はワールドへ固定する。ローカル量をそのままワールド量として持つ。
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
        m_driveImpulse = math::Vector3::ZERO;
    }

    void XPBDJoint::SolvePosition(float h)
    {
        if (!m_child) return;

        /// @note Gauss-Seidel は «後に解いたものが勝つ»。ドライブ (柔らかい目標) を先に、可動域
        /// @note (硬い壁) を次に、ソケット (伸びてはいけない) を最後に置く。逆順だと力み切った
        /// @note ドライブが関節を可動域の外へ押し出したまま残る。
        SolveDrive(h);
        SolveLimits(h);
        SolveSocket(h);
    }

    void XPBDJoint::SolvePhysicalPosition(float h)
    {
        if (!m_child) return;
        SolveDrive(h);
        if (m_limits.compliance>0.0f) SolveLimits(h);
        if (m_socketCompliance>0.0f) SolveSocket(h);
    }

    void XPBDJoint::SolveCorrectionPosition(float h)
    {
        if (!m_child) return;
        if (m_limits.compliance<=0.0f) SolveLimits(h);
        if (m_socketCompliance<=0.0f) SolveSocket(h);
    }

    void XPBDJoint::SolveDrive(float h)
    {
        m_driveSaturated = false;
        if (!m_drive.enabled || h<=0.0f) return;

        const math::Quaternion parentFrame = ParentFrameWorld();
        const math::Quaternion childFrame  = ChildFrameWorld();
        const math::Quaternion goal        = (parentFrame * m_drive.target).Normalized();

        /// @note 目標フレームから «今どれだけ回っているか»。子はこの逆へ回れば目標に着く。
        const math::Vector3 correction = RotationVector((childFrame * goal.Inverse()).Normalized());

        const float maxLambda = m_drive.maxTorque > 0.0f ? m_drive.maxTorque * h * h : 0.0f;
        const float deltaLambda=SolveAngular(m_child, m_parent, correction, m_drive.compliance, h, m_lambdaDrive, maxLambda);
        m_driveImpulse+=correction.NormalizedOr(math::Vector3::ZERO)*(deltaLambda/h);

        /// @note λ が上限に張り付いた ＝ 出したいトルクを出せていない ＝ 力負けしている。
        m_driveSaturated =
            maxLambda > 0.0f && std::abs(m_lambdaDrive) >= maxLambda * 0.9999f;
    }

    void XPBDJoint::SolveLimits(float h)
    {
        /// @note 診断値はドライブの後・ソケットの前で測る。可動域に対してどこに居るかを見たいので、
        /// @note «押し戻す前» の値を残す。
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

        /// @note スイング: 軸ごとの超過分をまとめて 1 回の角度補正にする。厳密には Y と Z の
        /// @note 回転は交換しないが、超過は小さい前提なので合成で足りる。
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

        /// @note ツイスト: スイングで傾いた «後» の骨の軸まわりに戻す。親フレームの X で回すと、
        /// @note 大きく振れているときに戻す向きがずれてスイングを増やしてしまう。
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

        /// @note SolveLimits が押し戻しの判定に使うのと同じ 3 つの角。
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
        const math::Vector3 pParent = m_parent ? m_parent->GetPosition() + rParent : m_anchorParent;

        SolvePositional(m_child, m_parent, rChild, rParent,
                        pChild - pParent, m_socketCompliance, h, m_lambdaSocket);
    }

    void XPBDJoint::SolveVelocity(float h)
    {
        if (!m_child || h<=0.0f) return;
        SolveDriveVelocity(h);
        ProjectVelocity(PrepareVelocityProjection());
    }

    XPBDJoint::VelocityProjection XPBDJoint::PrepareVelocityProjection() const
    {
        VelocityProjection projection;
        if (!m_child) return projection;
        /// @note 位置射影後の速度も関節の許容運動へ射影する。脱力中も接続点の速度を一致させ、自由回転は残す。
        /// @see https://matthias-research.github.io/pages/publications/PBDBodies.pdf Velocity Update.
        if (m_socketCompliance <= 0.0f) {
            projection.m_childArm = m_child->GetRotation() * m_anchorChild;
            projection.m_parentArm = m_parent ? m_parent->GetRotation() * m_anchorParent : math::Vector3::ZERO;
            const auto response = [&](const math::Vector3& impulse) {
                const auto& childArm = projection.m_childArm;
                const auto& parentArm = projection.m_parentArm;
                auto value = impulse * (m_child->GetInvMass() + (m_parent ? m_parent->GetInvMass() : 0.0f));
                value += math::Vector3::Cross(m_child->ApplyInvInertia(math::Vector3::Cross(childArm, impulse)), childArm);
                if (m_parent)
                    value += math::Vector3::Cross(m_parent->ApplyInvInertia(math::Vector3::Cross(parentArm, impulse)), parentArm);
                return value;
            };
            projection.m_columnX = response(math::Vector3::RIGHT);
            projection.m_columnY = response(math::Vector3::UP);
            projection.m_columnZ = response(math::Vector3::FORWARD);
            projection.m_cofactorX = math::Vector3::Cross(projection.m_columnY, projection.m_columnZ);
            projection.m_determinant = math::Vector3::Dot(projection.m_columnX, projection.m_cofactorX);
        }
        if (m_limits.enabled && m_limits.compliance <= 0.0f) {
            const auto parentFrame = ParentFrameWorld();
            const auto deviation = (parentFrame.Inverse() * ChildFrameWorld()).Normalized();
            math::Quaternion swing, twist;
            DecomposeSwingTwist(deviation, swing, twist);
            const auto swingVector = RotationVector(swing);
            const float twistAngle = TwistAngle(twist);
            constexpr float tolerance = 0.0001f;
            const auto atLimit = [](float angle, float low, float high) {
                return angle <= low + tolerance || angle >= high - tolerance;
            };
            const bool limitY = atLimit(swingVector.y, m_limits.swingMinY, m_limits.swingMaxY);
            const bool limitZ = atLimit(swingVector.z, m_limits.swingMinZ, m_limits.swingMaxZ);
            const bool limitTwist = atLimit(twistAngle, m_limits.twistMin, m_limits.twistMax);
            if (limitY || limitZ || limitTwist) {
                /// @note 速度射影の反復では姿勢が変わらない。上下限に達した軸を一度だけ用意する。
                const auto jacobians = BuildLimitJacobians(deviation);
                const auto addLimit = [&](const math::Vector3& gradient, float angle, float low, float high) {
                    projection.m_limits[projection.m_limitCount++] = {
                        (parentFrame * gradient).NormalizedOr(math::Vector3::ZERO),
                        angle <= low + tolerance,
                        angle >= high - tolerance,
                    };
                };
                if (limitY) addLimit(jacobians.swingY, swingVector.y, m_limits.swingMinY, m_limits.swingMaxY);
                if (limitZ) addLimit(jacobians.swingZ, swingVector.z, m_limits.swingMinZ, m_limits.swingMaxZ);
                if (limitTwist) addLimit(jacobians.twist, twistAngle, m_limits.twistMin, m_limits.twistMax);
            }
        }
        return projection;
    }

    void XPBDJoint::ProjectVelocity(const VelocityProjection& projection)
    {
        const bool projectSocket = projection.m_determinant > 1.0e-9f;
        if (!projectSocket && projection.m_limitCount == 0) return;
        const auto& rChild = projection.m_childArm;
        const auto& rParent = projection.m_parentArm;
        const auto& columnX = projection.m_columnX;
        const auto& columnY = projection.m_columnY;
        const auto& columnZ = projection.m_columnZ;
        const float determinant = projection.m_determinant;
        constexpr int VELOCITY_PROJECTION_ITERATIONS = 8;
        constexpr float SOCKET_VELOCITY_TOLERANCE_SQ = 1.0e-12f;
        const auto solveSocketImpulse = [&](const math::Vector3& relative) {
            return math::Vector3{
                math::Vector3::Dot(relative, projection.m_cofactorX) / determinant,
                math::Vector3::Dot(columnX, math::Vector3::Cross(relative, columnZ)) / determinant,
                math::Vector3::Dot(columnX, math::Vector3::Cross(columnY, relative)) / determinant};
        };
        const auto sameVelocity = [](const math::Vector3& a, const math::Vector3& b) {
            return std::bit_cast<std::uint32_t>(a.x) == std::bit_cast<std::uint32_t>(b.x)
                && std::bit_cast<std::uint32_t>(a.y) == std::bit_cast<std::uint32_t>(b.y)
                && std::bit_cast<std::uint32_t>(a.z) == std::bit_cast<std::uint32_t>(b.z);
        };
        const bool socketOnly = projection.m_limitCount == 0;
        const auto keepsLocalVelocity = [](const RigidBody& body) {
            const auto positionLock = body.GetFreezePosition();
            const auto rotationLock = body.GetFreezeRotation();
            return !body.m_allowSleeping && !body.IsSleeping()
                && std::bit_cast<std::uint32_t>(body.m_sleepTimer) == 0
                && !positionLock.x && !positionLock.y && !positionLock.z
                && !rotationLock.x && !rotationLock.y && !rotationLock.z;
        };
        /// @note 休止と軸固定の影響がない二剛体間では、関節内の反復結果だけを先に求めて一度書き戻す。
        if (socketOnly && m_parent && m_parent != m_child
            && keepsLocalVelocity(*m_child) && keepsLocalVelocity(*m_parent)) {
            const float childInverseMass = m_child->GetInvMass();
            const float parentInverseMass = m_parent->GetInvMass();
            auto childLinear = m_child->GetVelocity();
            auto childAngular = m_child->GetAngularVelocity();
            auto parentLinear = m_parent->GetVelocity();
            auto parentAngular = m_parent->GetAngularVelocity();
            for (int iteration = 0; iteration < VELOCITY_PROJECTION_ITERATIONS; ++iteration) {
                const auto childVelocity = childLinear + math::Vector3::Cross(childAngular, rChild);
                const auto parentVelocity = parentLinear + math::Vector3::Cross(parentAngular, rParent);
                const auto relative = childVelocity - parentVelocity;
                /// @note ソケットの 3×3 解は通常 1 回で収束する。残差が 1 µm/s 以下なら力積の再計算を省く。
                if (relative.LengthSq() <= SOCKET_VELOCITY_TOLERANCE_SQ) break;
                const auto impulse = solveSocketImpulse(relative);
                const auto nextChildLinear = childLinear - impulse * childInverseMass;
                const auto nextChildAngular = childAngular - m_child->ApplyInvInertia(math::Vector3::Cross(rChild, impulse));
                const auto nextParentLinear = parentLinear + impulse * parentInverseMass;
                const auto nextParentAngular = parentAngular + m_parent->ApplyInvInertia(math::Vector3::Cross(rParent, impulse));
                if (sameVelocity(nextChildLinear, childLinear) && sameVelocity(nextChildAngular, childAngular)
                    && sameVelocity(nextParentLinear, parentLinear) && sameVelocity(nextParentAngular, parentAngular))
                    break;
                childLinear = nextChildLinear;
                childAngular = nextChildAngular;
                parentLinear = nextParentLinear;
                parentAngular = nextParentAngular;
            }
            m_child->SetVelocity(childLinear);
            m_child->SetAngularVelocity(childAngular);
            m_parent->SetVelocity(parentLinear);
            m_parent->SetAngularVelocity(parentAngular);
            return;
        }
        /// @note 可動域とソケットの速度射影を交互に解く。ソケットだけで角速度を再生成して可動域へ突っ込ませない。
        for (int iteration = 0; iteration < VELOCITY_PROJECTION_ITERATIONS; ++iteration) {
            bool adjustedLimit = false;
            for (int index = 0; index < projection.m_limitCount; ++index) {
                const auto& limit = projection.m_limits[index];
                const auto relative = m_child->GetAngularVelocity() -
                    (m_parent ? m_parent->GetAngularVelocity() : math::Vector3::ZERO);
                const float speed = math::Vector3::Dot(relative, limit.axis);
                if ((limit.atUpper && speed > 0.0f) || (limit.atLower && speed < 0.0f)) {
                    ApplyAngularVelocityChange(m_child, m_parent, limit.axis * speed);
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
                const auto childVelocity = oldChildLinear + math::Vector3::Cross(oldChildAngular, rChild);
                const auto parentVelocity = m_parent ? oldParentLinear + math::Vector3::Cross(oldParentAngular, rParent) : math::Vector3::ZERO;
                const auto relative = childVelocity - parentVelocity;
                if (relative.LengthSq() <= SOCKET_VELOCITY_TOLERANCE_SQ) {
                    if (!adjustedLimit) break;
                    continue;
                }
                const auto impulse = solveSocketImpulse(relative);
                m_child->SetVelocity(m_child->GetVelocity()-impulse*m_child->GetInvMass());
                m_child->SetAngularVelocity(m_child->GetAngularVelocity()-m_child->ApplyInvInertia(math::Vector3::Cross(rChild,impulse)));
                if (m_parent) {
                    m_parent->SetVelocity(m_parent->GetVelocity()+impulse*m_parent->GetInvMass());
                    m_parent->SetAngularVelocity(m_parent->GetAngularVelocity()+m_parent->ApplyInvInertia(math::Vector3::Cross(rParent,impulse)));
                }
                /// @note ソケットだけを反復するとき、両者の速度と休止状態が同じなら次回も同じ結果になる。
                if (socketOnly && m_child->IsSleeping() == childSleeping
                    && sameVelocity(m_child->GetVelocity(), oldChildLinear)
                    && sameVelocity(m_child->GetAngularVelocity(), oldChildAngular)
                    && (!m_parent || (m_parent->IsSleeping() == parentSleeping
                        && sameVelocity(m_parent->GetVelocity(), oldParentLinear)
                        && sameVelocity(m_parent->GetAngularVelocity(), oldParentAngular))))
                    break;
            } else if (!adjustedLimit) break;
        }
    }

    void XPBDJoint::SolveDriveVelocity(float h)
    {
        if (!m_drive.enabled || m_drive.damping<=0.0f) return;

        const math::Vector3 parentOmega =
            m_parent ? m_parent->GetAngularVelocity() : math::Vector3::ZERO;
        const math::Vector3 relative = m_child->GetAngularVelocity() - parentOmega;

        /// @note 1 ステップで消せるのは «全部» まで。damping·h が 1 を超えると符号が反転して発散する。
        const float factor = std::min(m_drive.damping * h, 1.0f);
        const auto axis=relative.NormalizedOr(math::Vector3::ZERO);
        const float inverseMass=math::Vector3::Dot(axis,m_child->ApplyInvInertia(axis))
            +(m_parent ? math::Vector3::Dot(axis,m_parent->ApplyInvInertia(axis)) : 0.0f);
        if (inverseMass<=1.0e-9f) return;
        auto accumulated=m_driveImpulse-relative*(factor/inverseMass);
        /// @note 位置パスの λ/h と減衰を同じ角力積へ合算し、逆向きの減衰は既存の出力を撤回できる。
        /// @see https://box2d.org/posts/2024/02/solver2d/ Projected Gauss-Seidel の accumulated impulse。
        const float maxImpulse=m_drive.maxTorque*h;
        const float magnitude=accumulated.Length();
        if (maxImpulse>0.0f && magnitude>maxImpulse) accumulated=accumulated*(maxImpulse/magnitude);
        const auto impulse=accumulated-m_driveImpulse;
        m_child->SetAngularVelocity(m_child->GetAngularVelocity()+m_child->ApplyInvInertia(impulse));
        if (m_parent) m_parent->SetAngularVelocity(m_parent->GetAngularVelocity()-m_parent->ApplyInvInertia(impulse));
        m_driveImpulse=accumulated;
        /// @note 位置追従が既に出力不足なら、逆向きの減衰で合計出力が減ってもその substep の飽和診断を残す。
        m_driveSaturated=m_driveSaturated || (maxImpulse>0.0f && magnitude>=maxImpulse*0.9999f);
    }
} /// @note namespace fbzz::physics
