/// @file    RagdollRigTargets.cpp
/// @brief   アニメーション目標を剛体ドライブ・根のアンカー・関節へ反映する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include <Engine/Scene/Ragdoll/RagdollRig.hpp>
#include <Engine/Scene/Ragdoll/RagdollControls.hpp>
#include <Engine/Profiler/ProfileScope.hpp>

#include <Physics/Solver/XPBD/XPBDConstraint.hpp>

#include <cmath>

namespace fbzz::scene {

void RagdollRig::ResetDriveTargetHistory(const std::vector<RagdollBonePose>& bones)
{
    m_driveTargetHistory.Reset(bones, m_simulationTime);
    m_standingTargets = bones;
}

void RagdollRig::SampleDriveTargets(double simulationTime)
{
    if (!m_driveTargetHistory.HasSamples() || (!m_driveEnabled && !m_standingGuard)) {
        return;
    }
    FBZZ_PROFILE_SCOPE("RagdollRig::SampleDriveTargets");
    const auto sample = m_driveTargetHistory.Sample(simulationTime);
    ApplyTargetPose(sample.bones, nullptr, nullptr, 0.0f, sample.hasVelocities);
}

void RagdollRig::UpdateDriveTargets(const std::vector<RagdollBonePose>& bones, float sampleDt,
    const RagdollOwnerPose* ownerPose)
{
    if (!std::isfinite(sampleDt) || sampleDt < 0.0f) {
        return;
    }
    const auto& previousPose = m_driveTargetHistory.LatestPose();
    if (previousPose.size() == bones.size() && m_driveTargetHistory.NeedsRebase(m_simulationTime)) {
        ResetDriveTargetHistory(previousPose);
    }

    TransportStandingTargets(bones, ownerPose);
    m_hasPreviousOwnerPose = ownerPose != nullptr;
    if (ownerPose) {
        m_previousOwnerPose = *ownerPose;
    }

    if (sampleDt == 0.0f) {
        // 即時編集とActive再開は現在の物理時刻から始め、過去の停止時間を速度へ変換しない。
        ResetDriveTargetHistory(bones);
        ApplyTargetPose(bones, nullptr, nullptr, 0.0f);
        return;
    }
    if (previousPose.size() != bones.size()) {
        ResetDriveTargetHistory(bones);
    }
    const auto interval = m_driveTargetHistory.Append(bones, sampleDt);
    ApplyTargetPose(bones, &interval.before, &interval.after, sampleDt);
}

void RagdollRig::TransportStandingTargets(const std::vector<RagdollBonePose>& bones,
    const RagdollOwnerPose* ownerPose)
{
    const auto& previousPose = m_driveTargetHistory.LatestPose();
    if (!m_standingGuard || previousPose.size() != bones.size()) {
        return;
    }

    const auto transportBody = [](physics::RigidBody& body, const math::Vector3& previousOrigin,
        const math::Vector3& currentOrigin, const math::Quaternion& rotationDelta) {
        body.SetPosition(currentOrigin + rotationDelta * (body.GetPosition() - previousOrigin));
        body.SetRotation((rotationDelta * body.GetRotation()).Normalized());
        body.SetVelocity(rotationDelta * body.GetVelocity());
        body.SetAngularVelocity(rotationDelta * body.GetAngularVelocity());
    };
    // 腰アニメーションを所有者の移動と混同すると、接地中の足まで毎フレーム動かしてしまう。
    /// @see Docs/design/active-ragdoll.md
    if (ownerPose) {
        if (!m_hasPreviousOwnerPose) {
            return;
        }
        const auto rotationDelta = (ownerPose->rotation * m_previousOwnerPose.rotation.Inverse()).Normalized();
        for (auto& link : m_bodies) {
            transportBody(*link.body, m_previousOwnerPose.position, ownerPose->position, rotationDelta);
        }
        for (std::size_t boneIndex = 0; boneIndex < bones.size(); ++boneIndex) {
            if (bones[boneIndex].parent < 0) {
                m_driveTargetHistory.TransportRoot(
                    boneIndex, m_previousOwnerPose.position, ownerPose->position, rotationDelta);
            }
        }
        return;
    }

    for (auto& link : m_bodies) {
        int rootBoneIndex = link.boneIndex;
        while (bones[static_cast<std::size_t>(rootBoneIndex)].parent >= 0 &&
               bones[static_cast<std::size_t>(rootBoneIndex)].parent < rootBoneIndex) {
            rootBoneIndex = bones[static_cast<std::size_t>(rootBoneIndex)].parent;
        }
        const auto rootIndex = static_cast<std::size_t>(rootBoneIndex);
        const auto& previousRoot = previousPose[rootIndex];
        const auto& currentRoot = bones[rootIndex];
        const auto rotationDelta = (currentRoot.rotation * previousRoot.rotation.Inverse()).Normalized();
        transportBody(*link.body, previousRoot.position, currentRoot.position, rotationDelta);
    }
    for (std::size_t boneIndex = 0; boneIndex < bones.size(); ++boneIndex) {
        const auto& currentRoot = bones[boneIndex];
        if (currentRoot.parent >= 0) {
            continue;
        }
        const auto& previousRoot = previousPose[boneIndex];
        const auto rotationDelta = (currentRoot.rotation * previousRoot.rotation.Inverse()).Normalized();
        m_driveTargetHistory.TransportRoot(boneIndex, previousRoot.position, currentRoot.position, rotationDelta);
    }
}

void RagdollRig::ApplyTargetPose(const std::vector<RagdollBonePose>& bones,
    const std::vector<RagdollBonePose>* before, const std::vector<RagdollBonePose>* after, float sampleDt,
    bool useSampledVelocities)
{
    for (std::size_t boneIndex = 0; boneIndex < bones.size(); ++boneIndex) {
        m_standingTargets[boneIndex].position = bones[boneIndex].position;
        m_standingTargets[boneIndex].rotation = bones[boneIndex].rotation;
        m_standingTargets[boneIndex].scale = bones[boneIndex].scale;
    }
    ApplyBodyDriveTargets(bones, before, after, sampleDt, useSampledVelocities);
    ApplyRootAnchorTargets(bones);
    ApplyJointDriveTargets(bones);
}

void RagdollRig::ApplyBodyDriveTargets(const std::vector<RagdollBonePose>& bones,
    const std::vector<RagdollBonePose>* before, const std::vector<RagdollBonePose>* after, float sampleDt,
    bool useSampledVelocities)
{
    const auto resolveBodyTarget = [this](int bodyIndex, const std::vector<RagdollBonePose>& pose,
        math::Vector3& position, math::Quaternion& rotation,
        math::Vector3* linearVelocity = nullptr, math::Vector3* angularVelocity = nullptr) {
        const auto& link = m_bodies[static_cast<std::size_t>(bodyIndex)];
        const auto boneIndex = static_cast<std::size_t>(link.boneIndex);
        BodyPoseFromBone(bodyIndex, pose[boneIndex], position, rotation);
        if (linearVelocity && angularVelocity) {
            *angularVelocity = m_driveTargetHistory.SampledAngularVelocity(boneIndex);
            // 骨原点から重心へ移した速度には、固定オフセットの回転速度を差し引く。
            *linearVelocity = m_driveTargetHistory.SampledLinearVelocity(boneIndex)
                - math::Vector3::Cross(*angularVelocity, rotation * link.boneOffset);
        }
        if (m_controls->Space(link.name, link.activeDrive.space) == RagdollDriveSpace::PARENT &&
            link.parentBody >= 0) {
            const auto& parent = m_bodies[static_cast<std::size_t>(link.parentBody)];
            const auto parentBoneIndex = static_cast<std::size_t>(parent.boneIndex);
            math::Vector3 parentPosition;
            math::Quaternion parentRotation;
            BodyPoseFromBone(link.parentBody, pose[parentBoneIndex], parentPosition, parentRotation);
            const auto inverseParentRotation = parentRotation.Inverse();
            if (linearVelocity && angularVelocity) {
                const auto parentAngularVelocity = m_driveTargetHistory.SampledAngularVelocity(parentBoneIndex);
                const auto parentLinearVelocity = m_driveTargetHistory.SampledLinearVelocity(parentBoneIndex)
                    - math::Vector3::Cross(parentAngularVelocity, parentRotation * parent.boneOffset);
                // 親重心の移動と回転が運ぶ速度を除いてから、親の剛体空間へ変換する。
                *linearVelocity = inverseParentRotation * (*linearVelocity - parentLinearVelocity
                    - math::Vector3::Cross(parentAngularVelocity, position - parentPosition));
                *angularVelocity = inverseParentRotation * (*angularVelocity - parentAngularVelocity);
            }
            position = inverseParentRotation * (position - parentPosition);
            rotation = (inverseParentRotation * rotation).Normalized();
        }
    };
    for (std::size_t bodyIndex = 0; bodyIndex < m_bodies.size(); ++bodyIndex) {
        auto& link = m_bodies[bodyIndex];
        if (!link.drive || link.boneIndex < 0 || link.boneIndex >= static_cast<int>(bones.size())) {
            continue;
        }
        math::Vector3 position;
        math::Quaternion rotation;
        auto linearVelocity = math::Vector3::ZERO;
        auto angularVelocity = math::Vector3::ZERO;
        const bool useHistoryVelocity = useSampledVelocities && link.targetHistory;
        resolveBodyTarget(static_cast<int>(bodyIndex), bones, position, rotation,
            useHistoryVelocity ? &linearVelocity : nullptr, useHistoryVelocity ? &angularVelocity : nullptr);
        if (!useSampledVelocities && before && after && sampleDt > 0.0f && link.targetHistory) {
            math::Vector3 previousPosition, nextPosition;
            math::Quaternion previousRotation, nextRotation;
            resolveBodyTarget(static_cast<int>(bodyIndex), *before, previousPosition, previousRotation);
            resolveBodyTarget(static_cast<int>(bodyIndex), *after, nextPosition, nextRotation);
            linearVelocity = (nextPosition - previousPosition) / sampleDt;
            angularVelocity = physics::AngularVelocityFromDelta(previousRotation, nextRotation, sampleDt);
        }
        m_controls->BlendTarget(link.name, position, rotation, linearVelocity, angularVelocity);
        link.drive->SetTarget(position, rotation, linearVelocity, angularVelocity);
        link.targetHistory = true;
    }
}

void RagdollRig::ApplyRootAnchorTargets(const std::vector<RagdollBonePose>& bones)
{
    // 根には親との関節が無いため、アンカーでワールドのアニメーション姿勢へつなぎ止める。
    for (std::size_t anchorIndex = 0; anchorIndex < m_anchors.size(); ++anchorIndex) {
        const int bodyIndex = m_anchorBody[anchorIndex];
        const int boneIndex = m_bodies[static_cast<std::size_t>(bodyIndex)].boneIndex;
        if (boneIndex < 0 || boneIndex >= static_cast<int>(bones.size())) {
            continue;
        }

        math::Vector3 position;
        math::Quaternion rotation;
        BodyPoseFromBone(bodyIndex, bones[static_cast<std::size_t>(boneIndex)], position, rotation);
        m_anchors[anchorIndex]->SetTarget(position, rotation);
    }
}

void RagdollRig::ApplyJointDriveTargets(const std::vector<RagdollBonePose>& bones)
{
    // 目標は «この骨の姿勢なら関節はどれだけ曲がっているか»。剛体の «あるべき» 姿勢を
    // 骨から作り、関節フレームへ落として相対を取る。捕獲した姿勢と同じなら Identity に
    // なるので、無負荷での釣り合い点がそのままアニメーションになる。
    const auto rotationFromBone = [this, &bones](int bodyIndex, math::Quaternion& bodyRotation) {
        if (bodyIndex < 0 || bodyIndex >= static_cast<int>(m_bodies.size())) {
            return false;
        }
        const int boneIndex = m_bodies[static_cast<std::size_t>(bodyIndex)].boneIndex;
        if (boneIndex < 0 || boneIndex >= static_cast<int>(bones.size())) {
            return false;
        }

        math::Vector3 position;
        BodyPoseFromBone(bodyIndex, bones[static_cast<std::size_t>(boneIndex)], position, bodyRotation);
        return true;
    };

    for (JointLink& link : m_joints) {
        math::Quaternion parentRotation;
        math::Quaternion childRotation;
        if (!rotationFromBone(link.parentBody, parentRotation)) {
            continue;
        }
        if (!rotationFromBone(link.childBody, childRotation)) {
            continue;
        }

        const math::Quaternion parentFrame =
            (parentRotation * link.joint->GetFrameParent()).Normalized();
        const math::Quaternion childFrame =
            (childRotation * link.joint->GetFrameChild()).Normalized();
        const math::Quaternion target = (parentFrame.Inverse() * childFrame).Normalized();
        link.joint->Drive().target = target;

        if (!m_learnLimits) {
            continue;
        }

        // 角の測り方は関節側 (押し戻しに使うのと同じ分解) に任せる。学習側と検査側で
        // 分解や符号の扱いが 1 つでも違うと «広げたはずなのに押し戻される» という形でしか
        // 表面化しないため、測る関数を 1 つにして食い違いようが無い状態にする。
        link.joint->LearnLimits(target, m_limitMargin);
    }
}

} // namespace fbzz::scene
