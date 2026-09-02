/// @file    RagdollRig.cpp
/// @brief   骨の並びから剛体と関節を組み、姿勢を往復させる
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <Engine/Scene/Ragdoll/RagdollRig.hpp>

#include <Math/MathUtils.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::scene {

namespace {

constexpr float kMinBoneLength = 1.0e-4f;

math::Quaternion FromToRotation(const math::Vector3& from, const math::Vector3& to)
{
    const math::Vector3 f = from.NormalizedOr(math::Vector3::UP);
    const math::Vector3 t = to.NormalizedOr(math::Vector3::UP);
    const float d = math::Vector3::Dot(f, t);

    if (d >= 1.0f - math::EPSILON) return math::Quaternion::Identity();
    if (d <= -1.0f + math::EPSILON) {
        // 正反対。回転軸は f に直交していればどれでもよい。
        const math::Vector3 seed =
            std::abs(f.x) < 0.9f ? math::Vector3::RIGHT : math::Vector3::UP;
        return math::Quaternion::FromAxisAngle(
            math::Vector3::Cross(seed, f).NormalizedOr(math::Vector3::UP), math::PI);
    }

    const math::Vector3 axis = math::Vector3::Cross(f, t);
    return math::Quaternion{ axis.x, axis.y, axis.z, 1.0f + d }.Normalized();
}

/// base の指定軸が target を向くよう最小限だけ回す。骨の «ねじれ» を保ったまま
/// カプセルの向きだけを合わせるために使う。
math::Quaternion AlignAxis(const math::Quaternion& base,
                           const math::Vector3&    localAxis,
                           const math::Vector3&    targetWorld)
{
    return (FromToRotation(base * localAxis, targetWorld) * base).Normalized();
}

} // namespace

RagdollRig::~RagdollRig()
{
    Clear();
}

void RagdollRig::Clear()
{
    // 剛体より先にソルバから外す。ソルバは非所有ポインタで持っているので、
    // 順番を逆にすると «壊れた剛体の sleep フラグを書き戻す» ことになる。
    m_solver.ClearConstraints();
    m_solver.ClearBodies();
    m_joints.clear();
    m_bodies.clear();
    m_boneToBody.clear();
}

int RagdollRig::BodyIndexOfBone(int boneIndex) const
{
    if (boneIndex < 0 || boneIndex >= static_cast<int>(m_boneToBody.size())) return -1;
    return m_boneToBody[static_cast<std::size_t>(boneIndex)];
}

physics::RigidBody* RagdollRig::BodyOfBone(int boneIndex) const
{
    const int index = BodyIndexOfBone(boneIndex);
    return index < 0 ? nullptr : m_bodies[static_cast<std::size_t>(index)].body.get();
}

void RagdollRig::Build(const std::vector<RagdollBonePose>& bones, const RagdollProfile& profile)
{
    Clear();
    if (bones.empty()) return;

    const std::size_t count = bones.size();
    m_boneToBody.assign(count, -1);

    // 最初の子。剛体はここまでを 1 本のカプセルとして張る。
    std::vector<int> firstChild(count, -1);
    for (std::size_t i = 0; i < count; ++i) {
        const int parent = bones[i].parent;
        if (parent < 0 || parent >= static_cast<int>(count)) continue;
        if (firstChild[static_cast<std::size_t>(parent)] < 0)
            firstChild[static_cast<std::size_t>(parent)] = static_cast<int>(i);
    }

    for (std::size_t i = 0; i < count; ++i) {
        const int child = firstChild[i];
        if (child < 0) continue;   // 葉は剛体を持たない。親のカプセルに含める

        const math::Vector3 segment = bones[static_cast<std::size_t>(child)].position - bones[i].position;
        const float length = segment.Length();
        if (length <= kMinBoneLength) continue;

        const RagdollBoneSettings& settings = profile.Resolve(bones[i].name);
        const float radius     = std::max(settings.radius, kMinBoneLength);
        const float halfHeight = std::max(length * 0.5f - radius, kMinBoneLength);

        BodyLink link;
        link.boneIndex = static_cast<int>(i);
        link.collider  = std::make_unique<physics::CapsuleCollider>(radius, halfHeight);
        link.body      = std::make_unique<physics::RigidBody>();

        // CapsuleCollider の中心線はローカル Y。剛体の Y を骨の向きへ合わせる。
        // 関節フレーム (X = 骨) はこれとは別に XPBDJoint 側が持つので衝突しない。
        const math::Quaternion bodyRotation = AlignAxis(bones[i].rotation, math::Vector3::UP, segment);
        const math::Vector3    bodyPosition = bones[i].position + segment * 0.5f;

        const float mass = std::max(link.collider->ComputeVolume() * settings.density, 1.0e-3f);
        link.mass = mass;
        link.body->SetPosition(bodyPosition);
        link.body->SetRotation(bodyRotation);
        link.body->SetInertiaFromCollider(link.collider.get());
        link.body->SetMass(mass);

        // 骨 = 剛体 × これ。以後の捕獲も書き戻しもこの 2 つだけで往復する。
        const math::Quaternion inverseBody = bodyRotation.Inverse();
        link.boneOffset   = inverseBody * (bones[i].position - bodyPosition);
        link.boneRotation = (inverseBody * bones[i].rotation).Normalized();

        m_boneToBody[i] = static_cast<int>(m_bodies.size());
        m_bodies.push_back(std::move(link));
    }

    for (BodyLink& link : m_bodies) m_solver.AddBody(link.body.get());

    // 関節は «骨の位置» に立てる。親側の剛体は、骨の親を遡って最初に見つかったもの。
    for (std::size_t bodyIndex = 0; bodyIndex < m_bodies.size(); ++bodyIndex) {
        const BodyLink&   link      = m_bodies[bodyIndex];
        const std::size_t boneIndex = static_cast<std::size_t>(link.boneIndex);

        int parentBodyIndex = -1;
        for (int ancestor = bones[boneIndex].parent; ancestor >= 0;
             ancestor = bones[static_cast<std::size_t>(ancestor)].parent) {
            parentBodyIndex = BodyIndexOfBone(ancestor);
            if (parentBodyIndex >= 0) break;
        }
        if (parentBodyIndex < 0) continue;   // 根の剛体はワールドに繋がない (自由に落ちる)
        physics::RigidBody* parentBody = m_bodies[static_cast<std::size_t>(parentBodyIndex)].body.get();

        const int child = firstChild[boneIndex];
        const math::Vector3 segment =
            bones[static_cast<std::size_t>(child)].position - bones[boneIndex].position;

        auto joint = std::make_unique<physics::XPBDJoint>(parentBody, link.body.get());
        const RagdollBoneSettings& settings = profile.Resolve(bones[boneIndex].name);
        joint->Limits() = settings.limits;
        joint->Drive()  = settings.drive;
        // 関節フレームの X が骨の向き。可動域も目標姿勢もこのフレームで測る。
        joint->Build(bones[boneIndex].position,
                     AlignAxis(bones[boneIndex].rotation, math::Vector3::RIGHT, segment));

        m_joints.push_back(JointLink{ joint.get(), parentBodyIndex, static_cast<int>(bodyIndex) });
        m_solver.AddConstraint(std::move(joint));
    }
}

void RagdollRig::BodyPoseFromBone(int bodyIndex, const RagdollBonePose& bone,
                                  math::Vector3& outPosition, math::Quaternion& outRotation) const
{
    const BodyLink& link = m_bodies[static_cast<std::size_t>(bodyIndex)];
    outRotation = (bone.rotation * link.boneRotation.Inverse()).Normalized();
    outPosition = bone.position - outRotation * link.boneOffset;
}

void RagdollRig::Capture(const std::vector<RagdollBonePose>& bones)
{
    for (std::size_t i = 0; i < m_bodies.size(); ++i) {
        const BodyLink& link = m_bodies[i];
        if (link.boneIndex < 0 || link.boneIndex >= static_cast<int>(bones.size())) continue;

        math::Vector3    position;
        math::Quaternion rotation;
        BodyPoseFromBone(static_cast<int>(i), bones[static_cast<std::size_t>(link.boneIndex)],
                         position, rotation);

        link.body->SetPosition(position);
        link.body->SetRotation(rotation);
        // 初速は 0 から始める。どちらへ倒したいかは呼び出し側が押して決める。
        link.body->SetVelocity(math::Vector3::ZERO);
        link.body->SetAngularVelocity(math::Vector3::ZERO);
    }
}

void RagdollRig::UpdateDriveTargets(const std::vector<RagdollBonePose>& bones)
{
    // 目標は «この骨の姿勢なら関節はどれだけ曲がっているか»。剛体の «あるべき» 姿勢を
    // 骨から作り、関節フレームへ落として相対を取る。捕獲した姿勢と同じなら Identity に
    // なるので、無負荷での釣り合い点がそのままアニメーションになる。
    const auto rotationFromBone = [this, &bones](int bodyIndex, math::Quaternion& out) {
        if (bodyIndex < 0 || bodyIndex >= static_cast<int>(m_bodies.size())) return false;
        const int boneIndex = m_bodies[static_cast<std::size_t>(bodyIndex)].boneIndex;
        if (boneIndex < 0 || boneIndex >= static_cast<int>(bones.size())) return false;

        math::Vector3 position;
        BodyPoseFromBone(bodyIndex, bones[static_cast<std::size_t>(boneIndex)], position, out);
        return true;
    };

    for (JointLink& link : m_joints) {
        if (!link.joint->Drive().enabled) continue;

        math::Quaternion parentRotation;
        math::Quaternion childRotation;
        if (!rotationFromBone(link.parentBody, parentRotation)) continue;
        if (!rotationFromBone(link.childBody, childRotation)) continue;

        const math::Quaternion parentFrame =
            (parentRotation * link.joint->GetFrameParent()).Normalized();
        const math::Quaternion childFrame =
            (childRotation * link.joint->GetFrameChild()).Normalized();
        link.joint->Drive().target = (parentFrame.Inverse() * childFrame).Normalized();
    }
}

void RagdollRig::SetDriveEnabled(bool enabled)
{
    for (JointLink& link : m_joints) link.joint->Drive().enabled = enabled;
}

void RagdollRig::WritePose(const std::vector<RagdollBonePose>& fallback,
                           std::vector<math::Vector3>&         outPositions,
                           std::vector<math::Quaternion>&      outRotations) const
{
    const std::size_t count = fallback.size();
    outPositions.assign(count, math::Vector3::ZERO);
    outRotations.assign(count, math::Quaternion::Identity());

    // 親が先に並んでいる前提。剛体を持たない骨は «親からの相対» を保って埋めるので、
    // 親の結果が先に確定していなければならない。
    for (std::size_t i = 0; i < count; ++i) {
        const int bodyIndex = BodyIndexOfBone(static_cast<int>(i));
        if (bodyIndex >= 0) {
            const BodyLink& link = m_bodies[static_cast<std::size_t>(bodyIndex)];
            const math::Quaternion bodyRotation = link.body->GetRotation();
            outRotations[i] = (bodyRotation * link.boneRotation).Normalized();
            outPositions[i] = link.body->GetPosition() + bodyRotation * link.boneOffset;
            continue;
        }

        const int parent = fallback[i].parent;
        if (parent < 0 || parent >= static_cast<int>(count)) {
            outPositions[i] = fallback[i].position;
            outRotations[i] = fallback[i].rotation;
            continue;
        }

        const std::size_t p = static_cast<std::size_t>(parent);
        const math::Quaternion inverseParent = fallback[p].rotation.Inverse();
        const math::Quaternion localRotation = (inverseParent * fallback[i].rotation).Normalized();
        const math::Vector3    localPosition = inverseParent * (fallback[i].position - fallback[p].position);

        outRotations[i] = (outRotations[p] * localRotation).Normalized();
        outPositions[i] = outPositions[p] + outRotations[p] * localPosition;
    }
}

float RagdollRig::MeasureDeviation(const std::vector<RagdollBonePose>& bones) const
{
    float worst = 0.0f;
    for (std::size_t i = 0; i < m_bodies.size(); ++i) {
        const BodyLink& link = m_bodies[i];
        if (link.boneIndex < 0 || link.boneIndex >= static_cast<int>(bones.size())) continue;

        math::Vector3    position;
        math::Quaternion rotation;
        BodyPoseFromBone(static_cast<int>(i), bones[static_cast<std::size_t>(link.boneIndex)],
                         position, rotation);
        worst = std::max(worst, (link.body->GetPosition() - position).Length());
    }
    return worst;
}

math::Vector3 RagdollRig::CenterOfMass() const
{
    math::Vector3 weighted = math::Vector3::ZERO;
    float totalMass = 0.0f;
    for (const BodyLink& link : m_bodies) {
        weighted += link.body->GetPosition() * link.mass;
        totalMass += link.mass;
    }
    return totalMass > 0.0f ? weighted * (1.0f / totalMass) : math::Vector3::ZERO;
}

int RagdollRig::CountSaturatedJoints() const
{
    int count = 0;
    for (const JointLink& link : m_joints)
        if (link.joint->IsDriveSaturated()) ++count;
    return count;
}

} // namespace fbzz::scene
