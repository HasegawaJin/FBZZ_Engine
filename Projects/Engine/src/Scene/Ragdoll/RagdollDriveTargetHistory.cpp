/// @file    RagdollDriveTargetHistory.cpp
/// @brief   アニメーションの姿勢を保持し、物理時刻の骨姿勢と速度を復元する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include <Engine/Scene/Ragdoll/RagdollRig.hpp>

#include <Physics/Solver/XPBD/XPBDConstraint.hpp>

#include <algorithm>
#include <utility>

namespace fbzz::scene {

namespace {

constexpr double TARGET_TIME_TOLERANCE = 1.0e-7;

} /// @note namespace

void RagdollRig::DriveTargetHistory::Clear()
{
    m_samples.clear();
    m_firstPendingSample = 0;
    m_recycledBoneSamples.clear();
    m_latestSampleTime = 0.0;
    m_latestPose.clear();
    m_sampledWorldPose.clear();
    m_intervalStartPose.clear();
    m_intervalEndPose.clear();
    m_sampledLinearVelocities.clear();
    m_sampledAngularVelocities.clear();
}

bool RagdollRig::DriveTargetHistory::NeedsRebase(double simulationTime) const
{
    return !HasSamples() || m_latestSampleTime + TARGET_TIME_TOLERANCE < simulationTime;
}

void RagdollRig::DriveTargetHistory::RecycleSamples()
{
    for (std::size_t sampleIndex = m_firstPendingSample; sampleIndex < m_samples.size(); ++sampleIndex) {
        m_recycledBoneSamples.push_back(std::move(m_samples[sampleIndex].bones));
    }
    m_samples.clear();
    m_firstPendingSample = 0;
}

void RagdollRig::DriveTargetHistory::TransportRoot(std::size_t rootBoneIndex,
    const math::Vector3& previousOrigin, const math::Vector3& currentOrigin, const math::Quaternion& rotationDelta)
{
    for (std::size_t sampleIndex = m_firstPendingSample; sampleIndex < m_samples.size(); ++sampleIndex) {
        auto& rootPose = m_samples[sampleIndex].bones[rootBoneIndex];
        rootPose.position = currentOrigin + rotationDelta * (rootPose.position - previousOrigin);
        rootPose.rotation = (rotationDelta * rootPose.rotation).Normalized();
        rootPose.velocity = rotationDelta * rootPose.velocity;
        rootPose.angularVelocity = rotationDelta * rootPose.angularVelocity;
    }
}

void RagdollRig::DriveTargetHistory::Store(const std::vector<RagdollBonePose>& bones)
{
    if (m_firstPendingSample > 0 && m_firstPendingSample * 2 >= m_samples.size()) {
        m_samples.erase(m_samples.begin(), m_samples.begin() + m_firstPendingSample);
        m_firstPendingSample = 0;
    }
    TimedPose sample;
    sample.time = m_latestSampleTime;
    if (!m_recycledBoneSamples.empty()) {
        sample.bones = std::move(m_recycledBoneSamples.back());
        m_recycledBoneSamples.pop_back();
    }
    sample.bones.resize(bones.size());
    const float sampleDt = m_samples.empty() ? 0.0f
        : static_cast<float>(sample.time - m_samples.back().time);
    for (std::size_t boneIndex = 0; boneIndex < bones.size(); ++boneIndex) {
        const auto& bone = bones[boneIndex];
        auto& target = sample.bones[boneIndex];
        target.position = bone.position;
        target.rotation = bone.rotation;
        target.velocity = math::Vector3::ZERO;
        target.angularVelocity = math::Vector3::ZERO;
        if (bone.parent >= 0 && bone.parent < static_cast<int>(boneIndex)) {
            const auto& parent = bones[static_cast<std::size_t>(bone.parent)];
            const auto inverseParentRotation = parent.rotation.Inverse();
            target.position = inverseParentRotation * (bone.position - parent.position);
            target.rotation = (inverseParentRotation * bone.rotation).Normalized();
        }
        if (sampleDt > 0.0f) {
            const auto& previousBoneSample = m_samples.back().bones[boneIndex];
            target.velocity = (target.position - previousBoneSample.position) / sampleDt;
            /// @note Slerpの区間角速度を一度だけ保存し、小角近似やsubstep再差分による標本間隔依存を避ける。
            /// @see https://modernrobotics.northwestern.edu/nu-gm-book-resource/3-2-3-exponential-coordinates-of-rotation-part-1-of-2/ Modern Robotics §3.2.3 回転ベクトルと一定角速度。
            target.angularVelocity = physics::RotationVector(
                (target.rotation * previousBoneSample.rotation.Inverse()).Normalized()) / sampleDt;
        }
    }
    m_samples.push_back(std::move(sample));
}

void RagdollRig::DriveTargetHistory::Reset(const std::vector<RagdollBonePose>& bones, double simulationTime)
{
    RecycleSamples();
    m_latestSampleTime = simulationTime;
    m_latestPose = bones;
    m_sampledWorldPose = bones;
    m_intervalStartPose = bones;
    m_intervalEndPose = bones;
    m_sampledLinearVelocities.assign(bones.size(), math::Vector3::ZERO);
    m_sampledAngularVelocities.assign(bones.size(), math::Vector3::ZERO);
    Store(bones);
}

RagdollRig::DriveTargetHistory::PoseInterval RagdollRig::DriveTargetHistory::Append(
    const std::vector<RagdollBonePose>& bones, float sampleDt)
{
    m_latestSampleTime += static_cast<double>(sampleDt);
    m_latestPose = bones;
    Store(bones);
    ComposeWorldPose(m_samples[m_samples.size() - 2], m_intervalStartPose);
    ComposeWorldPose(m_samples.back(), m_intervalEndPose);
    return { m_intervalStartPose, m_intervalEndPose };
}

void RagdollRig::DriveTargetHistory::ComposeWorldPose(const TimedPose& sample,
    std::vector<RagdollBonePose>& worldPose) const
{
    for (std::size_t boneIndex = 0; boneIndex < sample.bones.size(); ++boneIndex) {
        auto& bone = worldPose[boneIndex];
        bone.position = sample.bones[boneIndex].position;
        bone.rotation = sample.bones[boneIndex].rotation;
        const int parentIndex = m_latestPose[boneIndex].parent;
        if (parentIndex >= 0 && parentIndex < static_cast<int>(boneIndex)) {
            const auto& parentPose = worldPose[static_cast<std::size_t>(parentIndex)];
            bone.position = parentPose.position + parentPose.rotation * bone.position;
            bone.rotation = (parentPose.rotation * bone.rotation).Normalized();
        }
    }
}

RagdollRig::DriveTargetHistory::SampledPose RagdollRig::DriveTargetHistory::Sample(double simulationTime)
{
    while (m_firstPendingSample + 2 < m_samples.size()
        && m_samples[m_firstPendingSample + 1].time < simulationTime - TARGET_TIME_TOLERANCE) {
        m_recycledBoneSamples.push_back(std::move(m_samples[m_firstPendingSample].bones));
        ++m_firstPendingSample;
    }
    const auto& before = m_samples[m_firstPendingSample];
    if (m_firstPendingSample + 1 == m_samples.size()) {
        ComposeWorldPose(before, m_sampledWorldPose);
        return { m_sampledWorldPose, false };
    }
    const auto& after = m_samples[m_firstPendingSample + 1];
    const double intervalDuration = after.time - before.time;
    const float interpolationWeight = intervalDuration > 0.0
        ? static_cast<float>(std::clamp((simulationTime - before.time) / intervalDuration, 0.0, 1.0)) : 1.0f;
    const bool useIntervalVelocity = intervalDuration > 0.0 && simulationTime <= after.time + TARGET_TIME_TOLERANCE;
    for (std::size_t boneIndex = 0; boneIndex < before.bones.size(); ++boneIndex) {
        const auto& startBone = before.bones[boneIndex];
        const auto& endBone = after.bones[boneIndex];
        auto& bone = m_sampledWorldPose[boneIndex];
        bone.position = math::Vector3::Lerp(startBone.position, endBone.position, interpolationWeight);
        bone.rotation = math::Quaternion::Slerp(startBone.rotation, endBone.rotation, interpolationWeight);
        auto& linearVelocity = m_sampledLinearVelocities[boneIndex];
        auto& angularVelocity = m_sampledAngularVelocities[boneIndex];
        linearVelocity = useIntervalVelocity ? endBone.velocity : math::Vector3::ZERO;
        angularVelocity = useIntervalVelocity ? endBone.angularVelocity : math::Vector3::ZERO;
        const int parentIndex = m_latestPose[boneIndex].parent;
        if (parentIndex >= 0 && parentIndex < static_cast<int>(boneIndex)) {
            const auto parent = static_cast<std::size_t>(parentIndex);
            const auto& parentPose = m_sampledWorldPose[parent];
            const auto worldOffset = parentPose.rotation * bone.position;
            /// @note 親の回転が子のworld位置を運ぶ速度を加え、同じ補間曲線の姿勢と速度を使う。
            /// @see https://modernrobotics.northwestern.edu/nu-gm-book-resource/3-3-2-twists-part-1-of-2/ Modern Robotics §3.3.2 回転による原点速度。
            linearVelocity = m_sampledLinearVelocities[parent]
                + math::Vector3::Cross(m_sampledAngularVelocities[parent], worldOffset)
                + parentPose.rotation * linearVelocity;
            angularVelocity = m_sampledAngularVelocities[parent] + parentPose.rotation * angularVelocity;
            bone.position = parentPose.position + worldOffset;
            bone.rotation = (parentPose.rotation * bone.rotation).Normalized();
        }
    }
    return { m_sampledWorldPose, true };
}

} /// @note namespace fbzz::scene
