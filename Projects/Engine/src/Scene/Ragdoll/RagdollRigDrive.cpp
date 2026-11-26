/// @file    RagdollRigDrive.cpp
/// @brief   剛体ドライブ・根のアンカー・関節サーボの強さとプロファイルを管理する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include <Engine/Scene/Ragdoll/RagdollRig.hpp>
#include <Engine/Scene/Ragdoll/RagdollControls.hpp>
#include <Engine/Profiler/ProfileScope.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace fbzz::scene {

namespace {

bool ValidBodyDrive(const RagdollBodyDrive& settings)
{
    const float values[] = {settings.linearFrequency, settings.angularFrequency, settings.linearDampingRatio,
        settings.angularDampingRatio, settings.maxForce, settings.maxTorque, settings.targetVelocityMultiplier};
    return (settings.space == RagdollDriveSpace::PARENT || settings.space == RagdollDriveSpace::WORLD)
        && std::all_of(std::begin(values), std::end(values), [](float value) { return std::isfinite(value) && value >= 0.0f; });
}

RagdollBodyDrive ResolveDrivePatch(const RagdollBodyDrive& base, const RagdollBodyDrivePatch& patch)
{
    return {
        patch.enabled.value_or(base.enabled),
        patch.space.value_or(base.space),
        patch.linearFrequency.value_or(base.linearFrequency),
        patch.angularFrequency.value_or(base.angularFrequency),
        patch.linearDampingRatio.value_or(base.linearDampingRatio),
        patch.angularDampingRatio.value_or(base.angularDampingRatio),
        patch.maxForce.value_or(base.maxForce),
        patch.maxTorque.value_or(base.maxTorque),
        patch.targetVelocityMultiplier.value_or(base.targetVelocityMultiplier),
    };
}

RagdollBodyDrive BlendBodyDrive(const RagdollBodyDrive& from, const RagdollBodyDrive& to, float weight)
{
    // on/off を切り替える間は出力上限を 0 から補間し、最後に目標の enabled と値へ揃える。
    return {
        from.enabled || to.enabled,
        to.space,
        std::lerp(from.linearFrequency, to.linearFrequency, weight),
        std::lerp(from.angularFrequency, to.angularFrequency, weight),
        std::lerp(from.linearDampingRatio, to.linearDampingRatio, weight),
        std::lerp(from.angularDampingRatio, to.angularDampingRatio, weight),
        std::lerp(from.enabled ? from.maxForce : 0.0f, to.enabled ? to.maxForce : 0.0f, weight),
        std::lerp(from.enabled ? from.maxTorque : 0.0f, to.enabled ? to.maxTorque : 0.0f, weight),
        std::lerp(from.targetVelocityMultiplier, to.targetVelocityMultiplier, weight),
    };
}

} // namespace

void RagdollRig::ApplyDrive()
{
    FBZZ_PROFILE_SCOPE("RagdollRig::ApplyDrive");
    const float gravityMagnitude = m_solver.GetGravity().Length();
    ApplyBodyDriveSettings();
    ApplyRootAnchorSettings(gravityMagnitude);
    ApplyJointServoSettings(gravityMagnitude);
}

void RagdollRig::ApplyBodyDriveSettings()
{
    for (auto& link : m_bodies) {
        if (!link.drive) {
            continue;
        }
        const auto& activeDrive = link.activeDrive;
        int jointDepth = 0;
        for (int ancestorBodyIndex = link.parentBody; ancestorBodyIndex >= 0;
             ancestorBodyIndex = m_bodies[static_cast<std::size_t>(ancestorBodyIndex)].parentBody) {
            ++jointDepth;
        }
        const float driveStrength = std::max(m_driveScale, 0.0f) *
            std::pow(std::max(m_driveFalloff, 0.0f), static_cast<float>(jointDepth));
        physics::XPBDBodyDriveSettings settings;
        settings.enabled = activeDrive.enabled && m_driveEnabled && driveStrength > 0.0f;
        settings.linearFrequency = activeDrive.linearFrequency * std::sqrt(driveStrength);
        settings.angularFrequency = activeDrive.angularFrequency * std::sqrt(driveStrength);
        settings.linearDampingRatio = activeDrive.linearDampingRatio * std::max(m_driveDamping, 0.0f);
        settings.angularDampingRatio = activeDrive.angularDampingRatio * std::max(m_driveDamping, 0.0f);
        settings.maxForce = activeDrive.maxForce * driveStrength;
        settings.maxTorque = activeDrive.maxTorque * driveStrength;
        settings.targetVelocityMultiplier = activeDrive.targetVelocityMultiplier;
        const auto driveSpace = m_controls->Space(link.name, activeDrive.space);
        link.drive->SetReference(driveSpace == RagdollDriveSpace::PARENT && link.parentBody >= 0
            ? m_bodies[static_cast<std::size_t>(link.parentBody)].body.get() : nullptr);
        link.drive->SetSettings(settings);
    }
}

void RagdollRig::ApplyRootAnchorSettings(float gravityMagnitude)
{
    // 繋ぎ止めもサーボと同じ «自重比» で持つ。重力を変えれば必要な力も変わる。
    const float anchorStrength = std::max(m_anchorScale, 0.0f);
    for (std::size_t anchorIndex = 0; anchorIndex < m_anchors.size(); ++anchorIndex) {
        const float supportedMass = anchorIndex < m_anchorMass.size() ? m_anchorMass[anchorIndex] : m_totalMass;
        const float massMoment = anchorIndex < m_anchorMoment.size() ? m_anchorMoment[anchorIndex] : 0.0f;
        auto& anchor = *m_anchors[anchorIndex];
        anchor.SetFiniteStrength(supportedMass * gravityMagnitude * anchorStrength,
            massMoment * gravityMagnitude * anchorStrength,
            std::max(m_anchorSag, 0.0f), std::max(m_anchorTilt, 0.0f));

        // XPBD の上限 0 は無制限を意味する。自重比から算出した出力 0 は明示的に切る。
        const auto& rootBody = m_bodies[static_cast<std::size_t>(m_anchorBody[anchorIndex])];
        anchor.SetEnabled(m_driveEnabled && anchorStrength > 0.0f && gravityMagnitude > 0.0f
            && !rootBody.authoredDrive && !rootBody.customDrive && !rootBody.profileDrive);
    }
}

void RagdollRig::ApplyJointServoSettings(float gravityMagnitude)
{
    for (JointLink& link : m_joints) {
        const float driveStrength =
            std::max(m_driveScale, 0.0f) *
            std::pow(std::max(m_driveFalloff, 0.0f), static_cast<float>(link.depth));

        physics::XPBDJointDrive& drive = link.joint->Drive();
        // 目標は毎フレーム別に更新される
        const math::Quaternion previousTarget = drive.target;

        drive = physics::XPBDJointDrive{};
        drive.target = previousTarget;
        const auto& childBody = m_bodies[static_cast<std::size_t>(link.childBody)];
        drive.enabled = link.baseServo.enabled && m_driveEnabled && driveStrength > 0.0f
            && !childBody.authoredDrive && !childBody.customDrive && !childBody.profileDrive;
        if (!drive.enabled) {
            continue;
        }

        // «その先を支えるのに要るトルク» に倍率を掛けたものが上限になる。骨格の
        // 大きさが変わっても «どれだけ余裕があるか» が保たれる (理由は RagdollServo を参照)。
        drive.maxTorque =
            link.holdMoment * gravityMagnitude * std::max(link.baseServo.torqueScale, 0.0f) * driveStrength;
        drive.enabled = drive.maxTorque > 0.0f;
        if (!drive.enabled) {
            continue;
        }
        // たわみ角 θ でのトルクは θ/α。α = holdSag / maxTorque と置くと、
        // «holdSag だけたわんだところで上限を出し切る» という一貫した意味になる。
        // 弱くすると上限が下がり、同じ式で compliance が上がる ─ 硬いまま上限だけ
        // 下げると、押した瞬間は耐えて限界で急に落ちる不連続な動きになる。
        drive.compliance =
            std::max(link.baseServo.holdSag, 0.0f) / std::max(drive.maxTorque, 1.0e-4f);
        drive.damping = link.baseServo.damping * std::max(m_driveDamping, 0.0f);
    }
}

void RagdollRig::SetEffectiveDrive(BodyLink& link, const RagdollBodyDrive& settings)
{
    if (link.activeDrive.space != settings.space) {
        link.targetHistory = false;
    }
    link.activeDrive = settings;
}

bool RagdollRig::SetBodyDrive(std::string_view bone, const RagdollBodyDrive& settings)
{
    if (!ValidBodyDrive(settings)) {
        return false;
    }
    for (auto& link : m_bodies) {
        if (link.name != bone) {
            continue;
        }
        SetEffectiveDrive(link, settings);
        link.customDrive = true;
        ResetRest();
        ApplyDrive();
        return true;
    }
    return false;
}

bool RagdollRig::ClearBodyDrive(std::string_view bone)
{
    for (auto& link : m_bodies) {
        if (link.name != bone) {
            continue;
        }
        link.customDrive = false;
        SetEffectiveDrive(link, link.profileDrive.value_or(link.baseDrive));
        ResetRest();
        ApplyDrive();
        return true;
    }
    return false;
}

bool RagdollRig::ApplyDriveProfile(std::span<const RagdollBodyDriveEntry> entries, float transitionSeconds)
{
    if (!std::isfinite(transitionSeconds) || transitionSeconds < 0.0f || entries.size() > m_bodies.size()) {
        return false;
    }
    std::vector<std::optional<RagdollBodyDrive>> profileTargets(m_bodies.size());
    for (const auto& entry : entries) {
        const auto body = std::find_if(m_bodies.begin(), m_bodies.end(),
            [&entry](const BodyLink& link) { return link.name == entry.bone; });
        if (entry.bone.empty() || body == m_bodies.end()) {
            return false;
        }
        auto& profileTarget = profileTargets[static_cast<std::size_t>(body - m_bodies.begin())];
        if (profileTarget) {
            return false;
        }
        profileTarget = ResolveDrivePatch(body->baseDrive, entry.patch);
        if (!ValidBodyDrive(*profileTarget)) {
            return false;
        }
    }

    m_driveProfileTransitions.clear();
    m_driveProfileElapsed = 0.0;
    m_driveProfileDuration = transitionSeconds;
    for (std::size_t bodyIndex = 0; bodyIndex < m_bodies.size(); ++bodyIndex) {
        auto& link = m_bodies[bodyIndex];
        const auto& profileTarget = profileTargets[bodyIndex];
        if (!profileTarget && !link.profileDrive) {
            continue;
        }
        if (transitionSeconds > 0.0f) {
            const auto currentDrive = link.profileDrive.value_or(link.baseDrive);
            m_driveProfileTransitions.push_back({ bodyIndex, currentDrive, profileTarget });
        } else {
            link.profileDrive = profileTarget;
            if (!link.customDrive) {
                SetEffectiveDrive(link, link.profileDrive.value_or(link.baseDrive));
            }
        }
    }
    ResetRest();
    ApplyDrive();
    return true;
}

void RagdollRig::AdvanceDriveProfile(float dt)
{
    if (m_driveProfileTransitions.empty()) {
        return;
    }
    m_driveProfileElapsed += static_cast<double>(dt);
    const bool transitionComplete = m_driveProfileElapsed >= m_driveProfileDuration;
    const float blendWeight = static_cast<float>(std::min(1.0, m_driveProfileElapsed / m_driveProfileDuration));
    for (const auto& transition : m_driveProfileTransitions) {
        auto& link = m_bodies[transition.bodyIndex];
        link.profileDrive = transitionComplete ? transition.to : std::optional{
            BlendBodyDrive(transition.from, transition.to.value_or(link.baseDrive), blendWeight) };
        if (!link.customDrive) {
            SetEffectiveDrive(link, link.profileDrive.value_or(link.baseDrive));
        }
    }
    if (transitionComplete) {
        m_driveProfileTransitions.clear();
    }
    ApplyDrive();
}

void RagdollRig::SetDrive(bool enabled, float scale, float falloff, float damping)
{
    if (enabled) {
        ResetRest();
    }
    m_driveEnabled = enabled;
    m_driveScale   = scale;
    m_driveFalloff = falloff;
    m_driveDamping = damping;
    // 脱力するときは繋ぎ止めも外す。残すと «力が抜けたのに胴だけ宙に留まる» になる。
    for (physics::XPBDPoseAnchor* anchor : m_anchors) {
        anchor->SetEnabled(enabled);
    }
    ApplyDrive();
}

void RagdollRig::SetRootAnchor(float scale, float sag, float tilt)
{
    m_anchorScale = scale;
    m_anchorSag   = sag;
    m_anchorTilt  = tilt;
    ApplyDrive();
}

void RagdollRig::SetLimitLearning(bool enabled, float margin)
{
    const bool wasEnabled = m_learnLimits;
    m_learnLimits = enabled;
    m_limitMargin = std::max(margin, 0.0f);

    if (!m_learnLimits || wasEnabled) {
        return;
    }

    // 有効にした «その瞬間の姿勢» は、可動域の内側であることにする。毎フレーム今の姿勢を
    // 取り込むと、崩れて押し込まれた角まで «許可された角» として焼き込み、可動域が時間と
    // ともに意味を失うため、false → true の遷移の 1 回だけにする
    // (RagdollSystem は毎フレーム同じ値で呼ぶ)。
    // 有効にするのはふつう捕獲の直後で、その姿勢が既に可動域の外まで曲がっていることが
    // ある。UpdateDriveTargets が目標から学習するより先に 1 ステップ回ると、そこで
    // 押し戻されて «学習が効く前に崩れる» ため、クリップの要求値ではなく «実際に置いた
    // 姿勢» を測る (両者がずれていても効く)。
    for (JointLink& link : m_joints) {
        link.joint->LearnLimitsFromCurrentPose(m_limitMargin);
    }
}

int RagdollRig::CountLimitedJoints() const
{
    int count = 0;
    for (const JointLink& link : m_joints)
        if (link.joint->IsLimited()) {
            ++count;
        }
    return count;
}

int RagdollRig::CountSaturatedJoints() const
{
    int count = 0;
    for (const JointLink& link : m_joints)
        if (link.joint->IsDriveSaturated()) {
            ++count;
        }
    for (const auto& link : m_bodies)
        if (link.drive && link.drive->IsSaturated()) {
            ++count;
        }
    return count;
}

} // namespace fbzz::scene
