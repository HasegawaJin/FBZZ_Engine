/// @file    RagdollRig.cpp
/// @brief   骨の並びから剛体と関節を組み、姿勢を往復させる
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <Engine/Scene/Ragdoll/RagdollRig.hpp>
#include <Engine/Scene/Ragdoll/RagdollControls.hpp>
#include <Engine/Scene/Ragdoll/StandingPose.hpp>
#include <Engine/Profiler/ProfileScope.hpp>

#include <Math/MathUtils.hpp>
#include <Physics/ColliderDebugGeometry.hpp>
#include <Physics/OBBCollider.hpp>
#include <Physics/PhysicsSolver.hpp>
#include <Physics/World.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace fbzz::scene {

namespace {

constexpr float kMinBoneLength = 1.0e-4f;

/// @note 箱の頂点球で近接点を探し、プローブの余白を除いた実形状の深さで支持を取る。
class RagdollBoxCollider final : public physics::OBBCollider {
public:
    explicit RagdollBoxCollider(const math::Vector3& extents) : physics::OBBCollider(extents)
    {
        const float radius=std::min({0.002f,extents.x*0.25f,extents.y*0.25f,extents.z*0.25f});
        m_cornerRadius=radius;
        std::size_t index=0;
        for (float x:{-1.0f,1.0f}) for (float y:{-1.0f,1.0f}) for (float z:{-1.0f,1.0f}) {
            m_corners[index].m_radius=radius;
            m_offsets[index]={x*(extents.x-radius),y*(extents.y-radius),z*(extents.z-radius)};
            ++index;
        }
    }
    std::array<physics::SphereCollider,8> m_corners{
        physics::SphereCollider(0.002f),physics::SphereCollider(0.002f),physics::SphereCollider(0.002f),physics::SphereCollider(0.002f),
        physics::SphereCollider(0.002f),physics::SphereCollider(0.002f),physics::SphereCollider(0.002f),physics::SphereCollider(0.002f)};
    std::array<math::Vector3,8> m_offsets;
    float m_cornerRadius=0.002f;
    float m_probeMargin=0.0f;
};

/// @note 隣の関節の射影で変わった速度を木全体へ伝える。有限モーターは再実行しない。
class RagdollJointVelocityProjection final : public physics::XPBDConstraint {
public:
    explicit RagdollJointVelocityProjection(std::vector<physics::XPBDContact>& contacts)
        : m_contacts(contacts)
    {
    }

    void AddJoint(physics::XPBDJoint& joint) { m_joints.push_back({&joint}); }
    void ResetLambda() override {}
    void SolvePosition(float) override {}
    void SolveVelocity(float h) override
    {
        if (m_joints.empty() || h <= 0.0f) return;
        FBZZ_PROFILE_SCOPE("RagdollRig::ProjectJointVelocities");
        /// @note このパスでは姿勢が固定され、Rig の剛体は休止しない。接触も速度だけを変える。
        for (auto& entry : m_joints) {
            entry.passive = !entry.joint->Drive().enabled;
            if (entry.passive) entry.prepared = entry.joint->PrepareVelocityProjection();
        }
        for (int pass = 0; pass < 4; ++pass) {
            for (auto& entry : m_joints)
                if (entry.passive) entry.joint->ProjectVelocity(entry.prepared);
            for (auto iterator = m_joints.rbegin(); iterator != m_joints.rend(); ++iterator)
                if (iterator->passive) iterator->joint->ProjectVelocity(iterator->prepared);
            /// @note 接触の速度補正と関節を交互に解き、小慣性の首・手へ残る接続点の誤差を減らす。
            /// @note 接触ごとに World の速度へ反映し、同じ物体に当たる次の部位へ更新済み速度を渡す。
            for (auto& contact : m_contacts) {
                contact.SolveVelocity(h);
                auto* other = contact.GetOther();
                if (!other || contact.IsOtherSolved()) continue;
                const auto impulse = contact.TakeReactionImpulse();
                if (!other->IsStatic() && impulse.LengthSq() > 0.0f)
                    other->ApplyImpulseAtPoint(impulse, contact.ContactPointWorld());
            }
        }
    }

private:
    struct JointProjection {
        physics::XPBDJoint* joint;
        physics::XPBDJoint::VelocityProjection prepared;
        bool passive = false;
    };

    std::vector<JointProjection> m_joints;
    std::vector<physics::XPBDContact>& m_contacts;
};

/// @note 無効な抜け止めの平面を仮の有限高さに置くと、その下へ移動した剛体まで押し戻してしまう。
constexpr float kNoGround = -std::numeric_limits<float>::infinity();

/// @note 抜け止めの平面が無効なら、全支持点の拘束を一度にスキップする。
class RagdollGroundPlane final : public physics::XPBDConstraint {
public:
    explicit RagdollGroundPlane(const float& height) : m_height(height) {}

    void AddContact(std::unique_ptr<physics::XPBDPlaneContact> contact)
    {
        m_contacts.push_back(std::move(contact));
    }

    void ResetLambda() override
    {
        if (m_height == kNoGround) return;
        for (auto& contact : m_contacts) contact->ResetLambda();
    }

    void SolvePosition(float h) override
    {
        if (m_height == kNoGround) return;
        for (auto& contact : m_contacts) contact->SolvePosition(h);
    }

    void SolvePhysicalPosition(float) override {}
    void SolveCorrectionPosition(float h) override { SolvePosition(h); }

    void SolveVelocity(float h) override
    {
        if (m_height == kNoGround) return;
        for (auto& contact : m_contacts) contact->SolveVelocity(h);
    }

private:
    const float& m_height;
    std::vector<std::unique_ptr<physics::XPBDPlaneContact>> m_contacts;
};

/// @note 1 フレームに解く接触の上限。倒れた関節体が地形の凹みへ嵌ると際限なく増えるので、
/// @note «増えるほど遅くなる» の上限だけ決めておく。深い順ではなく作られた順に採る ─
/// @note 並べ替えるコストの方が、たまに 1 点取りこぼす害より大きい。
constexpr std::size_t kMaxContacts = 256;
constexpr int kCoarseSupportSubsteps = 12;

bool IsFlatSupportContact(const physics::XPBDContact& contact, const physics::Collider* surface,
                          const math::Vector3& up)
{
    if (contact.IsOtherSolved() || (contact.GetOther() && !contact.GetOther()->IsStatic()) || !surface) return false;
    const auto type = surface->GetType();
    return (type == physics::ColliderType::AABB || type == physics::ColliderType::OBB)
        && math::Vector3::Dot(contact.GetNormal(), up) >= 1.0f - 1.0e-5f;
}

physics::AABB ExpandAABB(const physics::AABB& box, float margin)
{
    const math::Vector3 pad{ margin, margin, margin };
    return physics::AABB{ box.min - pad, box.max + pad };
}

math::Quaternion FromToRotation(const math::Vector3& from, const math::Vector3& to)
{
    const math::Vector3 f = from.NormalizedOr(math::Vector3::UP);
    const math::Vector3 t = to.NormalizedOr(math::Vector3::UP);
    const float d = math::Vector3::Dot(f, t);

    if (d >= 1.0f - math::EPSILON) return math::Quaternion::Identity();
    if (d <= -1.0f + math::EPSILON) {
        /// @note 正反対。回転軸は f に直交していればどれでもよい。
        const math::Vector3 seed =
            std::abs(f.x) < 0.9f ? math::Vector3::RIGHT : math::Vector3::UP;
        return math::Quaternion::FromAxisAngle(
            math::Vector3::Cross(seed, f).NormalizedOr(math::Vector3::UP), math::PI);
    }

    const math::Vector3 axis = math::Vector3::Cross(f, t);
    return math::Quaternion{ axis.x, axis.y, axis.z, 1.0f + d }.Normalized();
}

/// @note base の指定軸が target を向くよう最小限だけ回す。骨の «ねじれ» を保ったまま
/// @note カプセルの向きだけを合わせるために使う。
math::Quaternion AlignAxis(const math::Quaternion& base,
                           const math::Vector3&    localAxis,
                           const math::Vector3&    targetWorld)
{
    return (FromToRotation(base * localAxis, targetWorld) * base).Normalized();
}

} /// @note namespace

float RagdollRig::BodyLink::Bottom(const math::Vector3& p, const math::Quaternion& q) const
{
    if (shape == RagdollBodyShape::BOX) {
        return p.y - std::abs((q * math::Vector3::RIGHT).y)*halfExtents.x
            - std::abs((q * math::Vector3::UP).y)*halfExtents.y
            - std::abs((q * math::Vector3{0,0,1}).y)*halfExtents.z;
    }
    return p.y - std::abs((q * math::Vector3::UP).y)*halfHeight - radius;
}

RagdollRig::RagdollRig()
    : m_controls(std::make_unique<RagdollControls>(*this)), m_groundHeight(kNoGround)
{
}

RagdollRig::~RagdollRig()
{
    Clear();
}

void RagdollRig::Clear()
{
    /// @note 剛体より先にソルバから外す。ソルバは非所有ポインタで持っているので、
    /// @note 順番を逆にすると «壊れた剛体の sleep フラグを書き戻す» ことになる。
    m_solver.ClearTransient();
    m_controls->Reset();
    m_solver.ClearConstraints();
    m_solver.ClearBodies();
    m_joints.clear();
    m_ground.clear();
    m_anchors.clear();
    m_anchorBody.clear();
    m_anchorMass.clear();
    m_anchorMoment.clear();
    m_totalMass  = 0.0f;
    m_contacts.clear();
    m_contactCache.clear();
    m_previousContacts.clear();
    m_contactReactions.clear();
    m_contactWorld = nullptr;
    ResetRest();
    m_contactPoints.clear();
    m_pairs.clear();
    m_instances.clear();
    m_selfOverlapAtBuild.clear();
    m_unmatchedBones.clear();
    m_bodies.clear();
    m_driveProfileTransitions.clear();
    m_driveProfileElapsed = m_driveProfileDuration = 0.0;
    m_boneToBody.clear();
    m_standingTargets.clear();
    m_hasPreviousOwnerPose = false;
    m_driveTargetHistory.Clear();
    m_simulationTime = m_substepTargetTime = 0.0;
    m_standingPositions.clear();
    m_standingRotations.clear();
    m_standingLocalRotations.clear();
}

int RagdollRig::OwnBodyIndex(const physics::RigidBody* body) const
{
    if (!body) return -1;
    for (std::size_t i = 0; i < m_bodies.size(); ++i)
        if (m_bodies[i].body.get() == body) return static_cast<int>(i);
    return -1;
}

void RagdollRig::SetIgnoredColliders(std::vector<const physics::Collider*> colliders)
{
    m_ignoredColliders = std::move(colliders);
}

int RagdollRig::BodyIndexOfBone(int boneIndex) const
{
    if (boneIndex < 0 || boneIndex >= static_cast<int>(m_boneToBody.size())) return -1;
    return m_boneToBody[static_cast<std::size_t>(boneIndex)];
}

physics::RigidBody* RagdollRig::GetBody(int bodyIndex) const
{
    if (bodyIndex < 0 || bodyIndex >= static_cast<int>(m_bodies.size())) return nullptr;
    return m_bodies[static_cast<std::size_t>(bodyIndex)].body.get();
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
    for (const auto& definition:profile.bodies) {
        if (std::none_of(bones.begin(),bones.end(),[&](const auto& bone){return bone.name==definition.bone;}))
            m_unmatchedBones.push_back(definition.bone);
    }
    m_boneToBody.assign(count, -1);

    /// @note 最初の子。剛体はここまでを 1 本のカプセルとして張る。
    std::vector<int> firstChild(count, -1);
    for (std::size_t i = 0; i < count; ++i) {
        const int parent = bones[i].parent;
        if (parent < 0 || parent >= static_cast<int>(count)) continue;
        if (firstChild[static_cast<std::size_t>(parent)] < 0)
            firstChild[static_cast<std::size_t>(parent)] = static_cast<int>(i);
    }

    for (std::size_t i = 0; i < count; ++i) {
        const int child = firstChild[i];
        const auto* definition = profile.FindBody(bones[i].name);
        if (profile.definitionsOnly && !definition) continue;
        if (child < 0 && !definition) continue;
        /// @note Root / Armature のような «入れ物» は枝だけ辿る。剛体を作ると、子の骨まで
        /// @note 数 m 離れているぶんの棒が 1 本できてしまう。
        if (!definition && profile.IsBodyless(bones[i].name)) continue;

        const math::Vector3 segment = child >= 0 ? bones[static_cast<std::size_t>(child)].position - bones[i].position : bones[i].rotation * math::Vector3::UP * 0.1f;
        const float length = segment.Length();
        if (length <= kMinBoneLength && !definition) continue;

        bool matched = false;
        const RagdollBoneSettings& settings = profile.Resolve(bones[i].name, &matched);
        if (!matched) m_unmatchedBones.push_back(bones[i].name);
        const float radius = std::max({ length * settings.radiusRatio,
                                        settings.radiusMin, kMinBoneLength });
        const float halfHeight = std::max(length * 0.5f - radius, kMinBoneLength);

        BodyLink link;
        link.boneIndex = static_cast<int>(i);
        link.name = bones[i].name;
        link.authoredDrive = definition != nullptr;
        if (definition) link.baseDrive = link.activeDrive = definition->drive;
        link.shape = definition ? definition->shape : RagdollBodyShape::CAPSULE;
        link.radius = radius;
        link.halfHeight = halfHeight;
        if (definition) {
            const auto s = bones[i].scale;
            const float scale = std::max({std::abs(s.x),std::abs(s.y),std::abs(s.z)});
            link.radius = definition->radius * scale;
            link.halfHeight = definition->halfHeight * scale;
            link.halfExtents = definition->halfExtents * scale;
        }
        if (link.shape == RagdollBodyShape::BOX)
            link.collider = std::make_unique<RagdollBoxCollider>(link.halfExtents);
        else if (link.shape == RagdollBodyShape::SPHERE) {
            link.halfHeight = 0.0f;
            link.collider = std::make_unique<physics::SphereCollider>(link.radius);
        } else {
            link.collider = std::make_unique<physics::CapsuleCollider>(link.radius,link.halfHeight);
            link.caps[0] = std::make_unique<physics::SphereCollider>(link.radius);
            link.caps[1] = std::make_unique<physics::SphereCollider>(link.radius);
        }
        link.body      = std::make_unique<physics::RigidBody>();

        /// @note CapsuleCollider の中心線はローカル Y。剛体の Y を骨の向きへ合わせる。
        /// @note 関節フレーム (X = 骨) はこれとは別に XPBDJoint 側が持つので衝突しない。
        const math::Quaternion bodyRotation = definition
            ? (bones[i].rotation * definition->rotation).Normalized()
            : AlignAxis(bones[i].rotation, math::Vector3::UP, segment);
        const auto center = definition ? definition->center : math::Vector3::ZERO;
        const auto scale = bones[i].scale;
        const math::Vector3 bodyPosition = definition
            ? bones[i].position + bones[i].rotation * math::Vector3{center.x*scale.x,center.y*scale.y,center.z*scale.z}
            : bones[i].position + segment * 0.5f;

        const float mass = definition && definition->mass > 0.0f ? definition->mass : std::max(link.collider->ComputeVolume() * settings.density, 1.0e-3f);
        link.mass = mass;
        link.body->SetPosition(bodyPosition);
        link.body->SetRotation(bodyRotation);
        link.body->SetInertiaFromCollider(link.collider.get());
        link.body->SetMass(mass);
        if (definition && definition->inertia.LengthSq() > 0.0f) (void)link.body->SetInertiaDiagonal(definition->inertia);

        /// @note 骨 = 剛体 × これ。以後の捕獲も書き戻しもこの 2 つだけで往復する。
        const math::Quaternion inverseBody = bodyRotation.Inverse();
        link.boneOffset   = inverseBody * (bones[i].position - bodyPosition);
        link.boneRotation = (inverseBody * bones[i].rotation).Normalized();

        m_boneToBody[i] = static_cast<int>(m_bodies.size());
        m_bodies.push_back(std::move(link));
    }

    for (BodyLink& link : m_bodies) m_solver.AddBody(link.body.get());

    /// @note 親側の剛体は、骨の親を遡って最初に見つかったもの。
    for (std::size_t bodyIndex = 0; bodyIndex < m_bodies.size(); ++bodyIndex) {
        const std::size_t boneIndex = static_cast<std::size_t>(m_bodies[bodyIndex].boneIndex);
        for (int ancestor = bones[boneIndex].parent; ancestor >= 0;
             ancestor = bones[static_cast<std::size_t>(ancestor)].parent) {
            const int parentBodyIndex = BodyIndexOfBone(ancestor);
            if (parentBodyIndex < 0) continue;
            m_bodies[bodyIndex].parentBody = parentBodyIndex;
            break;
        }
    }

    /// @note 駆動を構造関節より先に解く。可動域と接触が最終的な形を制限する。
    for (auto& link : m_bodies) {
        auto drive=std::make_unique<physics::XPBDBodyDrive>(link.body.get());
        link.drive=drive.get();
        m_solver.AddConstraint(std::move(drive));
    }

    /// @note 親の関節を持たない剛体をアニメーションへ繋ぎ止める。一番柔らかいものを先に置く
    /// @note 規約 (後に解いた方が勝つ) のため関節より «先に» 解く。繋ぎ止めが関節や接触に
    /// @note 勝つと、骨が伸びてでも根が目標へ寄ることになる。
    for (std::size_t bodyIndex = 0; bodyIndex < m_bodies.size(); ++bodyIndex) {
        if (m_bodies[bodyIndex].parentBody >= 0) continue;
        auto anchor = std::make_unique<physics::XPBDPoseAnchor>(m_bodies[bodyIndex].body.get());
        m_anchors.push_back(anchor.get());
        m_anchorBody.push_back(static_cast<int>(bodyIndex));
        m_solver.AddConstraint(std::move(anchor));
    }

    /// @note 関節は «骨の位置» に立てる。
    std::vector<int> bodyDepth(m_bodies.size(), 0);
    for (std::size_t bodyIndex = 0; bodyIndex < m_bodies.size(); ++bodyIndex) {
        const BodyLink&   link      = m_bodies[bodyIndex];
        const std::size_t boneIndex = static_cast<std::size_t>(link.boneIndex);

        const int parentBodyIndex = link.parentBody;
        /// @note 根は関節ではなく繋ぎ止めで支える
        if (parentBodyIndex < 0) continue;
        physics::RigidBody* parentBody = m_bodies[static_cast<std::size_t>(parentBodyIndex)].body.get();

        /// @note 骨は親が先に並んでいるので、親側の剛体の段数は既に確定している。
        const int parentDepth = bodyDepth[static_cast<std::size_t>(parentBodyIndex)];
        bodyDepth[bodyIndex]  = parentDepth + 1;

        const int child = firstChild[boneIndex];
        const math::Vector3 segment = child >= 0
            ? bones[static_cast<std::size_t>(child)].position - bones[boneIndex].position
            : bones[boneIndex].rotation * math::Vector3::UP;
        const auto* definition = profile.FindBody(bones[boneIndex].name);

        auto joint = std::make_unique<physics::XPBDJoint>(parentBody, link.body.get());
        const RagdollBoneSettings& settings = profile.Resolve(bones[boneIndex].name);
        joint->Limits() = settings.limits;
        /// @note 関節フレームの X が骨の向き。可動域も目標姿勢もこのフレームで測る。
        const auto s = bones[boneIndex].scale;
        const auto p = definition ? definition->jointAnchor : math::Vector3::ZERO;
        joint->Build(bones[boneIndex].position + bones[boneIndex].rotation * math::Vector3{p.x*s.x,p.y*s.y,p.z*s.z},
                     definition ? (bones[boneIndex].rotation * definition->jointRotation).Normalized()
                                : AlignAxis(bones[boneIndex].rotation, math::Vector3::RIGHT, segment));

        JointLink jointLink;
        jointLink.joint      = joint.get();
        jointLink.parentBody = parentBodyIndex;
        jointLink.childBody  = static_cast<int>(bodyIndex);
        jointLink.depth      = parentDepth;
        jointLink.baseServo  = settings.servo;
        m_joints.push_back(jointLink);
        m_solver.AddConstraint(std::move(joint));
    }

    /// @note 各関節が «その先を支える» のに要るトルクの素。実トルクは これ × |g|。バインドポーズ
    /// @note では脚がまっすぐ下を向いていて重力に垂直な腕の長さがほぼ 0 になり、真横の姿勢を
    /// @note 基準にすると «必要トルク 0» と出てしまうため、姿勢に依らず決まる «関節からの
    /// @note 距離» を腕として採る。
    for (JointLink& link : m_joints) {
        const math::Vector3 anchor = link.joint->GetAnchorParentWorld();
        float moment = 0.0f;
        for (std::size_t b = 0; b < m_bodies.size(); ++b) {
            /// @note この関節に載っているのは childBody から先の部分木だけ。
            int ancestor = static_cast<int>(b);
            while (ancestor >= 0 && ancestor != link.childBody)
                ancestor = m_bodies[static_cast<std::size_t>(ancestor)].parentBody;
            if (ancestor != link.childBody) continue;
            moment += m_bodies[b].mass * (m_bodies[b].body->GetPosition() - anchor).Length();
        }
        link.holdMoment = moment;
    }

    /// @note 繋ぎ止めの強さの素。**根 1 本ごとに、その根がぶら下げている部分木だけ**を数える。
    /// @note 骨の並びは 1 本の木とは限らず、壊れた脚だけを落とす構成 (BossPolarityRigComponent)
    /// @note では繋がっていない脚が 4 本同じソルバに乗るため、全身の合計を全部の根へ配ると
    /// @note 脚 1 本の繋ぎ止めが «4 本ぶんを支える力» になり、脱力させたはずの脚がクリップへ
    /// @note 吸い付く。部分木ごとに数えれば、根が 1 本の従来の構成では合計と一致する。
    m_anchorMass.assign(m_anchors.size(), 0.0f);
    m_anchorMoment.assign(m_anchors.size(), 0.0f);
    for (std::size_t a = 0; a < m_anchorBody.size(); ++a) {
        const int anchorBody = m_anchorBody[a];
        const math::Vector3 root =
            m_bodies[static_cast<std::size_t>(anchorBody)].body->GetPosition();
        for (std::size_t b = 0; b < m_bodies.size(); ++b) {
            int ancestor = static_cast<int>(b);
            while (ancestor >= 0 && ancestor != anchorBody)
                ancestor = m_bodies[static_cast<std::size_t>(ancestor)].parentBody;
            if (ancestor != anchorBody) continue;
            m_anchorMass[a]   += m_bodies[b].mass;
            m_anchorMoment[a] += m_bodies[b].mass *
                                 (m_bodies[b].body->GetPosition() - root).Length();
        }
    }
    for (const BodyLink& link : m_bodies) m_totalMass += link.mass;

    auto velocityProjection = std::make_unique<RagdollJointVelocityProjection>(m_contacts);
    for (const auto& link : m_joints) velocityProjection->AddJoint(*link.joint);
    m_solver.AddConstraint(std::move(velocityProjection));

    /// @note 接地はカプセルの両端で取る。中心 1 点だと寝かせた胴が床へ半分めり込む。ソルバは
    /// @note 登録順に解き Gauss-Seidel は後に解いた方が勝つため、床は関節より後に足す:
    /// @note «骨がわずかに伸びてでも床から出る» になり、逆だと骨の長さを守るために
    /// @note 足が床へ沈んで接地が毎フレーム負ける。
    auto ground = std::make_unique<RagdollGroundPlane>(m_groundHeight);
    for (const BodyLink& link : m_bodies) {
        std::vector<math::Vector3> points;
        if (link.shape == RagdollBodyShape::BOX) {
            for (float x : {-1.0f,1.0f}) for (float y : {-1.0f,1.0f}) for (float z : {-1.0f,1.0f})
                points.push_back({x*link.halfExtents.x,y*link.halfExtents.y,z*link.halfExtents.z});
        } else {
            points.push_back({0,link.halfHeight,0});
            if (link.halfHeight > 0) points.push_back({0,-link.halfHeight,0});
        }
        for (const auto& point : points) {
            auto contact = std::make_unique<physics::XPBDPlaneContact>(
                link.body.get(), point, link.shape == RagdollBodyShape::BOX ? 0.0f : link.radius,
                math::Vector3::UP, m_groundHeight);
            contact->SetFriction(m_groundFriction);
            m_ground.push_back(contact.get());
            ground->AddContact(std::move(contact));
        }
    }
    m_solver.AddConstraint(std::move(ground));

    BuildShapeInstances();

    /// @note 組んだ時点で重なっている組は自己衝突から永久に外す。関節で繋がっていなくても
    /// @note 肩と胸のように «元から重ねてある» 組があり、当てると起動した瞬間に押し合って自壊する。
    const std::size_t bodyCount = m_bodies.size();
    m_selfOverlapAtBuild.assign(bodyCount * bodyCount, false);
    for (std::size_t a = 0; a < bodyCount; ++a) {
        m_bodies[a].collider->Update(m_bodies[a].body->GetPosition(),
                                     m_bodies[a].body->GetRotation());
    }
    for (std::size_t a = 0; a < bodyCount; ++a) {
        for (std::size_t b = a + 1; b < bodyCount; ++b) {
            if (!m_bodies[a].collider->GetAABB().Overlaps(m_bodies[b].collider->GetAABB()))
                continue;
            m_selfOverlapAtBuild[a * bodyCount + b] = true;
            m_selfOverlapAtBuild[b * bodyCount + a] = true;
        }
    }

    for (const auto& pair : profile.collisionExclusions) {
        int a=-1,b=-1;
        for (std::size_t i=0;i<m_bodies.size();++i) {
            const auto& name=bones[static_cast<std::size_t>(m_bodies[i].boneIndex)].name;
            if (name==pair.bodyA) a=static_cast<int>(i);
            if (name==pair.bodyB) b=static_cast<int>(i);
        }
        if (a>=0 && b>=0) {
            m_selfOverlapAtBuild[static_cast<std::size_t>(a)*bodyCount+static_cast<std::size_t>(b)]=true;
            m_selfOverlapAtBuild[static_cast<std::size_t>(b)*bodyCount+static_cast<std::size_t>(a)]=true;
        }
    }
    ApplyDrive();
}

/// @note 本体と頂点の近接プローブを一度だけ組み、以後は姿勢だけ同期する。自己衝突には本体だけを使う。
void RagdollRig::BuildShapeInstances()
{
    m_instances.clear();
    m_instances.reserve(m_bodies.size() * 9);

    for (BodyLink& link : m_bodies) {
        const float half = link.halfHeight;
        link.instanceIndex = m_instances.size();

        physics::ColliderInstance capsule;
        capsule.collider = link.collider.get();
        capsule.body     = link.body.get();
        capsule.material = &m_contactSettings.surface;
        m_instances.push_back(capsule);
        if (link.shape==RagdollBodyShape::BOX) {
            auto& box=static_cast<RagdollBoxCollider&>(*link.collider);
            for (std::size_t i=0;i<box.m_corners.size();++i) {
                auto corner=capsule;
                corner.collider=&box.m_corners[i];
                corner.centerOffset=box.m_offsets[i];
                m_instances.push_back(corner);
            }
        }

        for (int end = 0; end < 2; ++end) {
            if (!link.caps[end]) continue;
            physics::ColliderInstance cap;
            cap.collider     = link.caps[end].get();
            cap.body         = link.body.get();
            cap.material     = &m_contactSettings.surface;
            cap.centerOffset = { 0.0f, end == 0 ? half : -half, 0.0f };
            m_instances.push_back(cap);
        }
    }
}

void RagdollRig::RefreshContacts(physics::World* world)
{
    m_contactWorld = world;
    m_contactReactions.clear();
    RebuildContacts(world);
}

void RagdollRig::RebuildContacts(physics::World* world)
{
    FBZZ_PROFILE_SCOPE("RagdollRig::RebuildContacts");
    m_solver.ClearTransient();
    for (std::size_t i=0;i<m_contacts.size() && i<m_contactCache.size();++i) m_contactCache[i].contact=m_contacts[i];
    m_controls->RegisterGrabs(m_solver);
    m_previousContacts.swap(m_contactCache);
    m_contactCache.clear();
    for (auto& cached:m_previousContacts) cached.used=false;
    m_contacts.clear();
    m_contactPoints.clear();
    m_pairs.clear();
    if (m_instances.empty()) return;

    for (auto& link:m_bodies) if (link.shape==RagdollBodyShape::BOX) {
        auto& box=static_cast<RagdollBoxCollider&>(*link.collider);
        box.m_probeMargin=std::max(m_contactSettings.margin,0.0f);
        for (auto& corner:box.m_corners) corner.m_radius=box.m_cornerRadius+box.m_probeMargin;
    }

    /// @note 形状を今の姿勢へ同期する。NarrowPhase はコライダーが覚えているワールド形状を見るので、
    /// @note ここを飛ばすと «前フレームの位置で当たり判定する» ことになる。
    for (const physics::ColliderInstance& instance : m_instances) {
        const math::Quaternion rotation = instance.body->GetRotation();
        instance.collider->Update(
            instance.body->GetPosition() + rotation * instance.centerOffset, rotation);
    }

    if (world && (m_contactSettings.world || m_contactSettings.dynamic))
        CollectWorldPairs(*world);
    if (m_contactSettings.self)
        CollectSelfPairs();
    if (m_pairs.empty()) return;

    /// @note PhysicsSolver は状態を持たないので、呼ぶたびに作ってよい。
    physics::PhysicsSolver narrowPhase;
    {
        FBZZ_PROFILE_SCOPE("RagdollRig::NarrowPhase");
        narrowPhase.NarrowPhase(m_pairs, m_contactPoints);
    }
    MakeContacts();
}

void RagdollRig::CollectWorldPairs(physics::World& world)
{
    FBZZ_PROFILE_SCOPE("RagdollRig::CollectWorldPairs");
    m_controls->UpdateContactExclusions(world);
    /// @note 全身を包む球で 1 回だけ問い合わせる。OverlapSphere は全コライダーを線形に走査する
    /// @note ため、剛体ごと (骨 20 個) に引くと 20 往復で «コライダー総数 × 20» になり、
    /// @note アリーナ 1 枚を拾うために払う額として割に合わない。粗く 1 回拾ってから、
    /// @note こちらで AABB を突き合わせる方が安い。
    physics::AABB bounds = m_instances.front().collider->GetAABB();
    for (const physics::ColliderInstance& instance : m_instances)
        bounds = bounds.Merge(instance.collider->GetAABB());
    bounds = ExpandAABB(bounds, m_contactSettings.margin);

    const bool wantStatic  = m_contactSettings.world;
    const bool wantDynamic = m_contactSettings.dynamic;

    const std::vector<const physics::ColliderInstance*> candidates =
        world.OverlapSphere(bounds.Center(), bounds.Extents().Length(),
            [&](const physics::ColliderInstance& candidate) {
                if (!candidate.collider || candidate.isTrigger) return false;
                if (m_ignoredBody && candidate.body == m_ignoredBody) return false;
                if (m_controls->IgnoresContactsWith(candidate.body)) return false;
                /// @note 静的 = World が積分しないもの。地形も «剛体を持たないコライダー» なのでここ。
                const bool isStatic = !candidate.body || candidate.body->IsStatic();
                if (isStatic ? !wantStatic : !wantDynamic) return false;
                for (const physics::Collider* ignored : m_ignoredColliders)
                    if (ignored == candidate.collider) return false;
                return true;
            });
    if (candidates.empty()) return;

    for (std::size_t i = 0; i < m_instances.size(); ++i) {
        const physics::AABB own =
            ExpandAABB(m_instances[i].collider->GetAABB(), m_contactSettings.margin);
        for (const physics::ColliderInstance* candidate : candidates) {
            if (!own.Overlaps(candidate->collider->GetAABB())) continue;
            m_pairs.push_back(physics::CollisionPair{ &m_instances[i], candidate });
        }
    }
}

void RagdollRig::CollectSelfPairs()
{
    /// @note 自己衝突は各部位の本体だけを見る。補助球まで当てると同じ重なりを複数回
    /// @note 報告することになり、押し戻しがそのぶん硬くなる。
    /// @note BuildShapeInstances が «剛体 1 個につきカプセル → 端 → 端» の順で並べるので、
    /// @note 本体の ColliderInstance は各剛体の instanceIndex で引く。
    const std::size_t count = m_bodies.size();
    m_selfContactBounds.resize(count);
    for (std::size_t i = 0; i < count; ++i)
        m_selfContactBounds[i] = m_bodies[i].collider->GetAABB();

    for (std::size_t a = 0; a < count; ++a) {
        for (std::size_t b = a + 1; b < count; ++b) {
            if (!m_selfContactBounds[a].Overlaps(m_selfContactBounds[b])) continue;
            if (!AllowsSelfContact(static_cast<int>(a), static_cast<int>(b))) continue;
            m_pairs.push_back(physics::CollisionPair{ &m_instances[m_bodies[a].instanceIndex], &m_instances[m_bodies[b].instanceIndex] });
        }
    }
}

bool RagdollRig::AllowsSelfContact(int a, int b) const
{
    const std::size_t count = m_bodies.size();
    if (m_selfOverlapAtBuild.size() == count * count &&
        m_selfOverlapAtBuild[static_cast<std::size_t>(a) * count + static_cast<std::size_t>(b)])
        return false;

    /// @note 関節グラフ上の距離。先祖を selfSkip 段まで遡って相手に当たれば «繋がっている» 扱い。
    const int skip = std::max(m_contactSettings.selfSkip, 0);
    for (int side = 0; side < 2; ++side) {
        int from      = side == 0 ? a : b;
        const int to  = side == 0 ? b : a;
        for (int step = 0; step <= skip && from >= 0; ++step) {
            if (from == to) return false;
            from = m_bodies[static_cast<std::size_t>(from)].parentBody;
        }
    }
    return true;
}

void RagdollRig::MakeContacts()
{
    FBZZ_PROFILE_SCOPE("RagdollRig::MakeContacts");
    for (const physics::ContactPoint& point : m_contactPoints) {
        if (point.isTrigger) continue;
        if (m_contacts.size() >= kMaxContacts) break;

        const int indexA = OwnBodyIndex(point.bodyA);
        const int indexB = OwnBodyIndex(point.bodyB);
        if (indexA < 0 && indexB < 0) continue;

        /// @note 法線は B → A。押し出される側をこちらに揃えると、拘束は片側だけを見ればよくなる。
        physics::RigidBody* body   = indexA >= 0 ? point.bodyA : point.bodyB;
        physics::RigidBody* other  = indexA >= 0 ? point.bodyB : point.bodyA;
        math::Vector3 normal = indexA >= 0 ? point.normal : -point.normal;
        /// @note 相手も自分の骨なら双方を動かす。World が積分している剛体は動かさない
        /// @note (同じフレームで 2 回進んでしまう。反作用は ApplyContactReactions で返す)。
        const bool solveOther = indexA >= 0 && indexB >= 0;
        math::Vector3 worldPoint=point.point;
        float depth=point.depth;
        const auto& ownLink=m_bodies[static_cast<std::size_t>(indexA>=0 ? indexA : indexB)];
        const auto* ownCollider=indexA>=0 ? point.colliderA : point.colliderB;
        const auto* otherCollider = indexA >= 0 ? point.colliderB : point.colliderA;
        const bool externalBox = !solveOther && ownLink.shape == RagdollBodyShape::BOX;
        if (externalBox && ownCollider && ownCollider != ownLink.collider.get()
            && ownCollider->GetType() == physics::ColliderType::SPHERE) {
            const auto& box = static_cast<const RagdollBoxCollider&>(*ownLink.collider);
            const auto& probe = static_cast<const physics::SphereCollider&>(*ownCollider);
            const bool heightField = otherCollider && otherCollider->GetType() == physics::ColliderType::HEIGHT_FIELD;
            const bool crossedMeshFace = otherCollider && otherCollider->GetType() == physics::ColliderType::TRIANGLE_MESH
                && math::Vector3::Dot(normal, box.GetCenter() - worldPoint) < 0.0f;
            if (heightField || crossedMeshFace) {
                /// @note 面をまたいだ頂点の補助球は、箱本体と同じ側へ押し戻す。
                for (const auto& primary : m_contactPoints) {
                    const bool ownA = primary.bodyA == body && primary.colliderA == &box && primary.colliderB == otherCollider;
                    const bool ownB = primary.bodyB == body && primary.colliderB == &box && primary.colliderA == otherCollider;
                    if (!ownA && !ownB) continue;
                    const auto primaryNormal = ownA ? primary.normal : -primary.normal;
                    const float alignment = math::Vector3::Dot(normal, primaryNormal);
                    if (alignment >= 0.0f) break;
                    if (crossedMeshFace) {
                        /// @note メッシュ内の別の面を反転しないよう、SAT の押し戻し先と補助球の接触面を照合する。
                        const auto support = box.SupportPoint(-primaryNormal);
                        const float planeOffset = math::Vector3::Dot(worldPoint - support, primaryNormal) - primary.depth;
                        if (alignment > -0.9999f || std::abs(planeOffset) > 0.001f) break;
                    }
                    normal = -normal;
                    depth = 2.0f * probe.m_radius - depth;
                    break;
                }
            }
            depth -= box.m_probeMargin;
        }
        if (externalBox && ownCollider == ownLink.collider.get()) {
            /// @note NarrowPhase の支持面を1点へ潰さない。自身の頂点なら、各頂点の高さで貫通を評価する。
            const auto corners=static_cast<const physics::OBBCollider&>(*ownCollider).GetCorners();
            float lowest=math::Vector3::Dot(corners[0],normal);
            for (const auto& corner:corners) lowest=std::min(lowest,math::Vector3::Dot(corner,normal));
            bool projectToSurface=false;
            if (otherCollider && otherCollider->GetType()==physics::ColliderType::HEIGHT_FIELD) {
                projectToSurface=true;
                /// @note 地形の単点 SAT は支持面上の点を保証しないため、底面の支持位置で受ける。
                worldPoint=math::Vector3::ZERO;
                int count=0;
                for (const auto& corner:corners) {
                    if (math::Vector3::Dot(corner,normal)<=lowest+0.001f) { worldPoint+=corner; ++count; }
                }
                if (count>0) worldPoint=worldPoint*(1.0f/static_cast<float>(count));
            }
            for (const auto& corner:corners) {
                if ((corner-worldPoint).LengthSq()<1.0e-8f) {
                    depth-=math::Vector3::Dot(corner,normal)-lowest;
                    projectToSurface=true;
                    break;
                }
            }
            if (depth<=0.0f) continue;
            /// @note 自身の貫通頂点を相手の接触面へ戻す。IK と支持観測へ、床の内側の点を返さない。
            if (projectToSurface) worldPoint+=normal*depth;
        }

        const bool hasMaterials = point.materialA && point.materialB;
        const float friction = hasMaterials
            ? physics::PhysicsMaterial::CombineFriction(*point.materialA, *point.materialB)
            : m_contactSettings.surface.dynamicFriction;
        const float restitution = hasMaterials
            ? physics::PhysicsMaterial::CombineRestitution(*point.materialA, *point.materialB)
            : m_contactSettings.surface.restitution;

        const float staticFriction=hasMaterials
            ? physics::PhysicsMaterial::CombineStaticFriction(*point.materialA,*point.materialB)
            : m_contactSettings.surface.staticFriction;
        m_contacts.emplace_back();
        auto& contact=m_contacts.back();
        contact.Set(body,other,solveOther,worldPoint,normal,depth,friction,restitution);
        contact.SetStaticFriction(staticFriction);
        contact.SetContactSlop(std::min(m_contactSettings.margin,0.001f));
        CachedContact cached;
        cached.a=indexA>=0 ? point.colliderA : point.colliderB;
        cached.b=indexA>=0 ? point.colliderB : point.colliderA;
        cached.localPoint=body->GetRotation().Inverse()*(worldPoint-body->GetPosition());
        cached.otherPoint=other ? other->GetRotation().Inverse()*(worldPoint-other->GetPosition()) : worldPoint;
        cached.bodyA=body->ObserveLifetime();
        if (other) cached.bodyB=other->ObserveLifetime();
        if (cached.a) cached.lifetimeA=cached.a->ObserveLifetime();
        if (cached.b) cached.lifetimeB=cached.b->ObserveLifetime();
        float nearest=0.02f*0.02f;
        CachedContact* match=nullptr;
        for (auto& old:m_previousContacts) {
            if (old.used || old.a!=cached.a || old.b!=cached.b || old.bodyA.expired()
                || (other && old.bodyB.expired()) || (old.a && old.lifetimeA.expired())
                || (old.b && old.lifetimeB.expired())) continue;
            const float distance=(old.localPoint-cached.localPoint).LengthSq()
                +(old.otherPoint-cached.otherPoint).LengthSq();
            if (distance<nearest) { nearest=distance; match=&old; }
        }
        if (match) { contact.ContinueFrictionFrom(match->contact); match->used=true; }
        cached.contact=contact;
        m_contactCache.push_back(std::move(cached));
    }

    /// @note 実体が出揃ってからソルバへ渡す。作りながら渡すと、再確保で全部が宙を指す。
    for (physics::XPBDContact& contact : m_contacts) m_solver.AddTransient(&contact);
}

void RagdollRig::AccumulateContactReactions()
{
    for (physics::XPBDContact& contact : m_contacts) {
        physics::RigidBody* other = contact.GetOther();
        if (!other || contact.IsOtherSolved() || other->IsStatic()) continue;

        const math::Vector3 impulse = contact.TakeReactionImpulse();
        if (impulse.LengthSq() <= 0.0f) continue;
        m_contactReactions.push_back({ other, impulse, contact.ContactPointWorld() });
    }
}

void RagdollRig::ApplyContactReactions()
{
    AccumulateContactReactions();
    for (const auto& reaction : m_contactReactions)
        reaction.body->ApplyImpulseAtPoint(reaction.impulse, reaction.point);
    m_contactReactions.clear();
}

void RagdollRig::SetGround(float height, float friction)
{
    if (m_groundHeight == height && m_groundFriction == friction) return;
    const bool enabling = m_groundHeight == kNoGround;
    if (m_groundHeight != height) ResetRest();
    m_groundHeight   = height;
    m_groundFriction = friction;
    for (physics::XPBDPlaneContact* contact : m_ground) {
        if (enabling) contact->ResetHistory();
        contact->SetPlane(math::Vector3::UP, height);
        contact->SetFriction(friction);
    }
}

void RagdollRig::DisableGround()
{
    if (m_groundHeight == kNoGround) return;
    m_groundHeight = kNoGround;
    ResetRest();
}

float RagdollRig::LowestContactHeight(const std::vector<RagdollBonePose>& bones) const
{
    float lowest = 0.0f;
    bool  found  = false;

    for (std::size_t i = 0; i < m_bodies.size(); ++i) {
        const BodyLink& link = m_bodies[i];
        if (link.boneIndex < 0 || link.boneIndex >= static_cast<int>(bones.size())) continue;

        math::Vector3    position;
        math::Quaternion rotation;
        BodyPoseFromBone(static_cast<int>(i), bones[static_cast<std::size_t>(link.boneIndex)],
                         position, rotation);

        const float y = link.Bottom(position,rotation);
        if (!found || y < lowest) { lowest=y; found=true; }
    }
    return lowest;
}

void RagdollRig::ApplyImpulse(const math::Vector3& origin,
                              const math::Vector3& velocity,
                              float                radius)
{
    if (velocity.LengthSq() > 0.0f) ResetRest();
    for (BodyLink& link : m_bodies) {
        float scale = 1.0f;
        if (radius > 0.0f) {
            const float distance = (link.body->GetPosition() - origin).Length();
            scale = 1.0f - math::Clamp01(distance / radius);
            if (scale <= 0.0f) continue;
        }
        link.body->SetVelocity(link.body->GetVelocity() + velocity * scale);
    }
}

void RagdollRig::ApplyAngularVelocity(const math::Vector3& origin,
                                      const math::Vector3& angularVelocity, float radius)
{
    if (angularVelocity.LengthSq() > 0.0f) ResetRest();
    for (const auto& link : m_bodies) {
        const float scale = radius > 0.0f
            ? 1.0f - math::Clamp01((link.body->GetPosition() - origin).Length() / radius) : 1.0f;
        link.body->SetAngularVelocity(link.body->GetAngularVelocity() + angularVelocity * scale);
    }
}

void RagdollRig::SetDrag(float linear, float angular)
{
    for (BodyLink& link : m_bodies) {
        link.body->m_linearDrag  = std::max(linear, 0.0f);
        link.body->m_angularDrag = std::max(angular, 0.0f);
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
    ResetRest();
    m_solver.ClearTransient();
    m_contacts.clear();
    m_contactCache.clear();
    m_previousContacts.clear();
    for (auto& link:m_bodies) link.targetHistory=false;
    m_hasPreviousOwnerPose = false;
    m_standingTargets = bones;
    for (std::size_t i = 0; i < m_bodies.size(); ++i) {
        const BodyLink& link = m_bodies[i];
        if (link.boneIndex < 0 || link.boneIndex >= static_cast<int>(bones.size())) continue;

        math::Vector3    position;
        math::Quaternion rotation;
        BodyPoseFromBone(static_cast<int>(i), bones[static_cast<std::size_t>(link.boneIndex)],
                         position, rotation);

        link.body->SetPosition(position);
        link.body->SetRotation(rotation);
        /// @note 初速は 0 から始める。どちらへ倒したいかは呼び出し側が押して決める。
        link.body->SetVelocity(math::Vector3::ZERO);
        link.body->SetAngularVelocity(math::Vector3::ZERO);
    }

    /// @note 剛体を «瞬間移動» させたので、接触が覚えている前フレームの位置は無効。
    for (physics::XPBDPlaneContact* contact : m_ground) contact->ResetHistory();
    m_simulationTime = 0.0;
    ResetDriveTargetHistory(bones);
    /// @note 捕獲だけでは駆動目標を更新しない。最初のUpdateDriveTargetsで現在の物理時刻から補間を開始する。
    m_driveTargetHistory.RecycleSamples();
}

bool RagdollRig::TryGetBoneSupport(std::string_view bone, math::Vector3& outPosition,
    math::Vector3& outPoint, math::Vector3& outNormal, const physics::Collider*& outCollider) const
{
    if (!m_driveEnabled) return false;
    for (const auto& link:m_bodies) {
        if (link.name!=bone) continue;
        for (std::size_t i=0;i<m_contacts.size() && i<m_contactCache.size();++i) {
            const auto& contact=m_contacts[i];
            const auto& cached=m_contactCache[i];
            if (cached.lifetimeB.expired()) continue;
            if (contact.GetBody()!=link.body.get() || !contact.IsTouching()
                || OwnBodyIndex(contact.GetOther())>=0 || contact.GetNormal().y<0.5f) continue;
            outPosition=link.body->GetPosition()+link.body->GetRotation()*link.boneOffset;
            outPoint=contact.ContactPointWorld();
            outNormal=contact.GetNormal();
            outCollider=cached.b;
            return true;
        }
        return false;
    }
    return false;
}

bool RagdollRig::TryGetFootPlacement(std::string_view bone, const math::Quaternion& boneRotation,
    const math::Vector3& normal, math::Quaternion& outRotation, float& outHeight,
    const math::Vector3* soleNormal) const
{
    if (!std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z)
        || normal.LengthSq() < 0.5f || normal.y <= 0.25f) return false;
    if (!std::isfinite(boneRotation.Length()) || boneRotation.Length()<0.5f) return false;
    const auto n=normal.Normalized();
    const auto up=soleNormal ? soleNormal->NormalizedOr(n) : n;
    if (!std::isfinite(up.x) || !std::isfinite(up.y) || !std::isfinite(up.z)) return false;
    for (const auto& link:m_bodies) {
        if (link.name!=bone) continue;
        auto bodyRotation=(boneRotation*link.boneRotation.Inverse()).Normalized();
        bodyRotation=(FromToRotation(bodyRotation*math::Vector3::UP,up)*bodyRotation).Normalized();
        const auto offset=bodyRotation*link.boneOffset;
        float reach=link.radius;
        if (link.shape==RagdollBodyShape::BOX) {
            reach=std::abs(math::Vector3::Dot(bodyRotation*math::Vector3::RIGHT,n))*link.halfExtents.x
                +std::abs(math::Vector3::Dot(bodyRotation*math::Vector3::UP,n))*link.halfExtents.y
                +std::abs(math::Vector3::Dot(bodyRotation*math::Vector3::FORWARD,n))*link.halfExtents.z;
        } else reach+=std::abs(math::Vector3::Dot(bodyRotation*math::Vector3::UP,n))*link.halfHeight;
        outHeight=(reach+math::Vector3::Dot(offset,n))/n.y;
        outRotation=(bodyRotation*link.boneRotation).Normalized();
        return true;
    }
    return false;
}

void RagdollRig::SetStandingGuard(bool enabled, float maxDistance, float maxRadians)
{
    if (m_standingGuard != enabled) m_hasPreviousOwnerPose = false;
    m_standingGuard = enabled;
    m_standingMaxDistance = std::max(maxDistance, 0.0f);
    m_standingMaxRadians = std::max(maxRadians, 0.0f);
}

void RagdollRig::ProjectStandingPose()
{
    if (m_standingTargets.size() != m_boneToBody.size()) return;
    FBZZ_PROFILE_SCOPE("RagdollRig::ProjectStandingPose");
    WritePose(m_standingTargets, m_standingPositions, m_standingRotations);
    LimitStandingPose(m_standingTargets, m_standingMaxDistance, m_standingMaxRadians,
                      m_standingPositions, m_standingRotations, m_standingLocalRotations);
    for (std::size_t i = 0; i < m_bodies.size(); ++i) {
        const auto boneIndex = static_cast<std::size_t>(m_bodies[i].boneIndex);
        RagdollBonePose pose;
        pose.position = m_standingPositions[boneIndex];
        pose.rotation = m_standingRotations[boneIndex];
        math::Vector3 position;
        math::Quaternion rotation;
        BodyPoseFromBone(static_cast<int>(i), pose, position, rotation);
        m_standingPositions[boneIndex] = position;
        m_standingRotations[boneIndex] = rotation;
    }

    /// @note 上限の外から戻るときは固着を緩め、接地を維持したまま復帰が止まるのを防ぐ。
    const float recoverySlip = std::max(0.0f, MeasureDeviation(m_standingTargets) - m_standingMaxDistance);
    /// @note ガードは静的な支持面を越えない範囲まで。全身に共通の割合を使い、足だけを切り離さない。
    const bool coarseSupport = m_solver.GetSubsteps() <= kCoarseSupportSubsteps;
    const auto up = -m_solver.GetGravity().NormalizedOr(-math::Vector3::UP);
    const auto preservesSupport = [&](float weight) {
        for (std::size_t i = 0; i < m_contacts.size(); ++i) {
            const auto& contact = m_contacts[i];
            if (contact.IsOtherSolved() || (contact.GetOther() && !contact.GetOther()->IsStatic())) continue;
            const int index = OwnBodyIndex(contact.GetBody());
            if (index < 0) continue;
            const auto& link = m_bodies[static_cast<std::size_t>(index)];
            const auto boneIndex = static_cast<std::size_t>(link.boneIndex);
            const auto position = math::Vector3::Lerp(link.body->GetPosition(), m_standingPositions[boneIndex], weight);
            const auto rotation = math::Quaternion::Slerp(link.body->GetRotation(), m_standingRotations[boneIndex], weight);
            /// @note 法線が一定の支持面では、固着残差の境界で全身の補正率が跳ねないようにする。
            const bool smoothSlipTolerance = coarseSupport && IsFlatSupportContact(contact, m_contactCache[i].b, up);
            if (!contact.AllowsPoseCorrection(position, rotation, recoverySlip, smoothSlipTolerance)) return false;
        }
        return true;
    };
    float weight = 1.0f;
    if (!preservesSupport(weight)) {
        weight = 0.0f;
        float upper = 1.0f;
        for (int i = 0; i < 8; ++i) {
            const float middle = (weight + upper) * 0.5f;
            if (preservesSupport(middle)) weight = middle;
            else upper = middle;
        }
    }
    if (weight == 0.0f) return;
    for (const auto& link : m_bodies) {
        const auto boneIndex = static_cast<std::size_t>(link.boneIndex);
        const auto& position = m_standingPositions[boneIndex];
        const auto& rotation = m_standingRotations[boneIndex];
        link.body->SetPosition(weight == 1.0f ? position : math::Vector3::Lerp(link.body->GetPosition(), position, weight));
        link.body->SetRotation(weight == 1.0f ? rotation : math::Quaternion::Slerp(link.body->GetRotation(), rotation, weight));
    }
}

void RagdollRig::CaptureContactPoses()
{
    for (auto& link : m_bodies) {
        link.previousPosition = link.body->GetPosition();
        link.previousRotation = link.body->GetRotation();
    }
}

void RagdollRig::UpdateRest(float dt)
{
    if (m_controls->HasGrabs() || !m_contactSettings.allowRest || m_driveEnabled || m_standingGuard || dt <= 0.0f || m_bodies.empty()) {
        ResetRest();
        return;
    }

    bool supported = false;
    bool dynamicContact = false;
    const auto up = -m_solver.GetGravity().NormalizedOr(-math::Vector3::UP);
    for (const auto& contact : m_contacts) {
        if (!contact.IsTouching()) continue;
        const int ownA = OwnBodyIndex(contact.GetBody());
        const int ownB = OwnBodyIndex(contact.GetOther());
        if ((ownA >= 0) == (ownB >= 0)) continue;
        const auto* other = ownA >= 0 ? contact.GetOther() : contact.GetBody();
        if (other && !other->IsStatic()) { dynamicContact = true; continue; }
        const auto normal = ownA >= 0 ? contact.GetNormal() : -contact.GetNormal();
        supported = supported || math::Vector3::Dot(normal, up) > 0.25f;
    }
    if (m_groundHeight != kNoGround) {
        for (const auto& link : m_bodies) {
            const float bottom = link.Bottom(link.body->GetPosition(),link.body->GetRotation());
            supported = supported || bottom <= m_groundHeight + 0.002f;
        }
    }

    float motion = 0.0f;
    float peakMotion = 0.0f;
    float mass = 0.0f;
    bool moved = false;
    for (const auto& link : m_bodies) {
        const auto delta = link.body->GetPosition() - link.previousPosition;
        const auto angular = physics::AngularVelocityFromDelta(
            link.previousRotation, link.body->GetRotation(), dt);
        const float reach = link.Reach();
        const float surfaceMotion=delta.LengthSq()/(dt*dt)+angular.LengthSq()*reach*reach;
        motion += link.mass * surfaceMotion;
        peakMotion=std::max(peakMotion,surfaceMotion);
        mass += link.mass;
        moved = moved || delta.LengthSq() > 0.005f * 0.005f || angular.LengthSq() * dt * dt > 0.02f * 0.02f;
    }
    /// @note 個々の骨は sleep させず、支持された Passive の関節体全体を質量加重の表面速度で判定する。
    /// @note 接触解決は休止中も続け、支持の消失・動的接触・姿勢の変化で再開する。
    if (!supported || dynamicContact || mass <= 0.0f || motion / mass > 0.05f * 0.05f || peakMotion > 0.15f * 0.15f || (m_resting && moved)) {
        ResetRest();
        return;
    }
    if (!m_resting) {
        m_restTime += dt;
        if (m_restTime < 0.75f) return;
        CaptureContactPoses();
        m_resting = true;
    }
    for (const auto& link : m_bodies) {
        link.body->SetPosition(link.previousPosition);
        link.body->SetRotation(link.previousRotation);
        link.body->SetVelocity(math::Vector3::ZERO);
        link.body->SetAngularVelocity(math::Vector3::ZERO);
    }
}

void RagdollRig::Step(float dt)
{
    FBZZ_PROFILE_SCOPE("RagdollRig::Step");
    if (!std::isfinite(dt) || dt <= 0.0f) return;
    AdvanceDriveProfile(dt);
    m_controlSubstep=dt/static_cast<float>(m_solver.GetSubsteps());
    m_substepTargetTime = m_simulationTime;
    if (!m_resting) CaptureContactPoses();
    /// @note フレーム間の移動で薄い地形を通過する前に、積分後の形状から接触を作り直す。
    /// @see https://matthias-research.github.io/pages/publications/smallsteps.pdf Small Steps in Physics Simulation, §3 の substep 積分と拘束解決。
    /// @note 接触から複数関節先まで硬い拘束の補正を伝え、脱力した手足の残留振動を抑える。
    const auto up = -m_solver.GetGravity().NormalizedOr(-math::Vector3::UP);
    const bool coarsePassiveSupport = !m_driveEnabled && !m_standingGuard && !m_controls->HasGrabs()
        && m_solver.GetSubsteps() <= kCoarseSupportSubsteps
        && std::any_of(m_contactCache.begin(), m_contactCache.end(), [&](const CachedContact& cached) {
            return IsFlatSupportContact(cached.contact, cached.b, up);
        });
    const int correctionIterations = coarsePassiveSupport ? 12 : 8;
    /// @note 立位制限なしでは、ソルバの投影前後の姿勢保存と速度制限が不要。
    const auto projectStanding = m_standingGuard ? +[](void* context) {
        static_cast<RagdollRig*>(context)->ProjectStandingPose();
    } : nullptr;
    m_solver.Step(dt, [](void* context) {
        auto& rig = *static_cast<RagdollRig*>(context);
        FBZZ_PROFILE_SCOPE("RagdollRig::PrepareSubstep");
        rig.m_substepTargetTime += static_cast<double>(rig.m_controlSubstep);
        rig.SampleDriveTargets(rig.m_substepTargetTime);
        rig.m_controls->IntegrateForces(rig.m_controlSubstep);
        /// @note 次の substep は相手の反応済み速度を読む。フレーム末まで溜めると軽い物体へ同じ衝撃を重ねる。
        rig.ApplyContactReactions();
        rig.RebuildContacts(rig.m_contactWorld);
    }, projectStanding, this, physics::XPBDProjectionMode::LIMIT, correctionIterations);
    m_simulationTime += static_cast<double>(dt);
    UpdateRest(dt);
    m_contactWorld = nullptr;
}

void RagdollRig::WritePose(const std::vector<RagdollBonePose>& fallback,
                           std::vector<math::Vector3>&         outPositions,
                           std::vector<math::Quaternion>&      outRotations) const
{
    const std::size_t count = fallback.size();
    outPositions.assign(count, math::Vector3::ZERO);
    outRotations.assign(count, math::Quaternion::Identity());

    /// @note 親が先に並んでいる前提。剛体を持たない骨は «親からの相対» を保って埋めるので、
    /// @note 親の結果が先に確定していなければならない。
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

void RagdollRig::BuildDebugLines(std::vector<RagdollDebugLine>& out) const
{
    using Kind = RagdollDebugLine::Kind;
    const auto push = [&out](const math::Vector3& from, const math::Vector3& to, Kind kind) {
        out.push_back(RagdollDebugLine{ from, to, kind });
    };

    for (const BodyLink& link : m_bodies) {
        /// @note 接触探索時の姿勢は拘束を解く前のため、形状のコピーを解決後の剛体姿勢で描く。
        const auto drawBody = [&](auto collider) {
            collider.Update(link.body->GetPosition(), link.body->GetRotation());
            const auto geometry = physics::BuildColliderDebugGeometry(collider);
            for (const auto& line : geometry.lines)
                push(line.from, line.to, Kind::Body);
        };
        switch (link.shape) {
        case RagdollBodyShape::BOX:
            drawBody(static_cast<const physics::OBBCollider&>(*link.collider));
            break;
        case RagdollBodyShape::SPHERE:
            drawBody(static_cast<const physics::SphereCollider&>(*link.collider));
            break;
        case RagdollBodyShape::CAPSULE:
            drawBody(static_cast<const physics::CapsuleCollider&>(*link.collider));
            break;
        }
    }

    for (const JointLink& link : m_joints) {
        const physics::XPBDJoint& joint = *link.joint;
        const BodyLink& child = m_bodies[static_cast<std::size_t>(link.childBody)];
        const math::Vector3 anchor = joint.GetAnchorParentWorld();
        const math::Quaternion frame = joint.GetParentFrameWorld();
        const physics::XPBDJointLimits& limits = joint.Limits();
        const Kind kind = joint.IsDriveSaturated() || (child.drive && child.drive->IsSaturated())
            ? Kind::JointSaturated : Kind::Joint;

        const float length = std::max(child.Reach(), 0.05f);
        const auto childAnchor = child.body->GetPosition() + child.body->GetRotation() * joint.GetAnchorChild();

        /// @note 拘束で両支点が離れても、中立の X 軸と子の X 軸はそれぞれの支点から描く。
        push(anchor, anchor + frame * math::Vector3::RIGHT * length, kind);
        push(childAnchor, childAnchor + joint.GetChildFrameWorld() * math::Vector3::RIGHT * (length * 1.2f), kind);
        if (!limits.enabled) continue;

        /// @note XPBDJoint::SolveLimits はスイングを単一の回転ベクトルで測る。
        const auto swingTip = [&](const math::Vector3& rotationVector) {
            const auto swing = math::Quaternion::FromAxisAngle(
                rotationVector.NormalizedOr(math::Vector3::UP), rotationVector.Length());
            return anchor + (frame * swing) * math::Vector3::RIGHT * length;
        };
        const auto drawSwingEdge = [&](const math::Vector3& fixed, const math::Vector3& axis, float from, float to) {
            /// @note 最短回転は π まで。境界円上はすべて関節の −X に写る。
            const float remaining = math::PI * math::PI - fixed.LengthSq();
            if (remaining < 0.0f) return;
            const float reach = std::sqrt(remaining);
            if (std::min(from, to) > reach || std::max(from, to) < -reach) return;
            const float start = std::clamp(from, -reach, reach);
            const float end = std::clamp(to, -reach, reach);
            auto previous = swingTip(fixed + axis * start);
            push(anchor, previous, kind);

            /// @note 広い屈曲域でも端同士を直線でつないで可動域を横切らないようにする。
            constexpr float MAX_SEGMENT_ANGLE = 15.0f * math::DEG2RAD;
            const int segments = static_cast<int>(std::ceil(std::abs(end - start) / MAX_SEGMENT_ANGLE));
            for (int i = 1; i <= segments; ++i) {
                const float fraction = static_cast<float>(i) / static_cast<float>(segments);
                const auto tip = swingTip(fixed + axis * std::lerp(start, end, fraction));
                push(previous, tip, kind);
                previous = tip;
            }
        };
        drawSwingEdge(math::Vector3::FORWARD * limits.swingMinZ, math::Vector3::UP, limits.swingMinY, limits.swingMaxY);
        drawSwingEdge(math::Vector3::UP * limits.swingMaxY, math::Vector3::FORWARD, limits.swingMinZ, limits.swingMaxZ);
        drawSwingEdge(math::Vector3::FORWARD * limits.swingMaxZ, math::Vector3::UP, limits.swingMaxY, limits.swingMinY);
        drawSwingEdge(math::Vector3::UP * limits.swingMinY, math::Vector3::FORWARD, limits.swingMaxZ, limits.swingMinZ);
    }

    for (const physics::XPBDContact& contact : m_contacts) {
        if (contact.GetPenetration() <= 0.0f) continue;
        const math::Vector3 point = contact.ContactPointWorld();
        push(point, point + contact.GetNormal() * 0.25f, Kind::Contact);
    }
}

} /// @note namespace fbzz::scene
