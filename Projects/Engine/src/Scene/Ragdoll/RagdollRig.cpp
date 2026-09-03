/// @file    RagdollRig.cpp
/// @brief   骨の並びから剛体と関節を組み、姿勢を往復させる
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <Engine/Scene/Ragdoll/RagdollRig.hpp>

#include <Math/MathUtils.hpp>
#include <Physics/ColliderDebugGeometry.hpp>
#include <Physics/PhysicsSolver.hpp>
#include <Physics/World.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace fbzz::scene {

namespace {

constexpr float kMinBoneLength = 1.0e-4f;

/// 1 フレームに解く接触の上限。倒れた関節体が地形の凹みへ嵌ると際限なく増えるので、
/// «増えるほど遅くなる» の上限だけ決めておく。深い順ではなく作られた順に採る ─
/// 並べ替えるコストの方が、たまに 1 点取りこぼす害より大きい。
constexpr std::size_t kMaxContacts = 256;

/// 接地面を «届かない所» へ置いておくための高さ。SetGround を呼ぶまで、あるいは
/// DisableGround の後は床が無いのと同じになる。拘束を後から足せないソルバなので、
/// 器だけ先に用意して面を下げる形にしてある。
constexpr float kNoGround = -1.0e9f;

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
    m_solver.ClearTransient();
    m_solver.ClearConstraints();
    m_solver.ClearBodies();
    m_joints.clear();
    m_ground.clear();
    m_anchors.clear();
    m_anchorBody.clear();
    m_rootMoment = 0.0f;
    m_totalMass  = 0.0f;
    m_contacts.clear();
    m_contactPoints.clear();
    m_pairs.clear();
    m_instances.clear();
    m_selfOverlapAtBuild.clear();
    m_unmatchedBones.clear();
    m_bodies.clear();
    m_boneToBody.clear();
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
        // Root / Armature のような «入れ物» は枝だけ辿る。剛体を作ると、子の骨まで
        // 数 m 離れているぶんの棒が 1 本できてしまう。
        if (profile.IsBodyless(bones[i].name)) continue;

        const math::Vector3 segment = bones[static_cast<std::size_t>(child)].position - bones[i].position;
        const float length = segment.Length();
        if (length <= kMinBoneLength) continue;

        bool matched = false;
        const RagdollBoneSettings& settings = profile.Resolve(bones[i].name, &matched);
        if (!matched) m_unmatchedBones.push_back(bones[i].name);
        const float radius = std::max({ length * settings.radiusRatio,
                                        settings.radiusMin, kMinBoneLength });
        const float halfHeight = std::max(length * 0.5f - radius, kMinBoneLength);

        BodyLink link;
        link.boneIndex = static_cast<int>(i);
        link.collider  = std::make_unique<physics::CapsuleCollider>(radius, halfHeight);
        link.caps[0]   = std::make_unique<physics::SphereCollider>(radius);
        link.caps[1]   = std::make_unique<physics::SphereCollider>(radius);
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

    // 親側の剛体は、骨の親を遡って最初に見つかったもの。
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

    // 親の関節を持たない剛体をアニメーションへ繋ぎ止める。
    //
    // WHY 関節より «先に» 解くか: 一番柔らかいものを先に置く規約 (後に解いた方が勝つ)。
    //     繋ぎ止めが関節や接触に勝つと、骨が伸びてでも根が目標へ寄ることになる。
    for (std::size_t bodyIndex = 0; bodyIndex < m_bodies.size(); ++bodyIndex) {
        if (m_bodies[bodyIndex].parentBody >= 0) continue;
        auto anchor = std::make_unique<physics::XPBDPoseAnchor>(m_bodies[bodyIndex].body.get());
        m_anchors.push_back(anchor.get());
        m_anchorBody.push_back(static_cast<int>(bodyIndex));
        m_solver.AddConstraint(std::move(anchor));
    }

    // 関節は «骨の位置» に立てる。
    std::vector<int> bodyDepth(m_bodies.size(), 0);
    for (std::size_t bodyIndex = 0; bodyIndex < m_bodies.size(); ++bodyIndex) {
        const BodyLink&   link      = m_bodies[bodyIndex];
        const std::size_t boneIndex = static_cast<std::size_t>(link.boneIndex);

        const int parentBodyIndex = link.parentBody;
        if (parentBodyIndex < 0) continue;   // 根は関節ではなく繋ぎ止めで支える
        physics::RigidBody* parentBody = m_bodies[static_cast<std::size_t>(parentBodyIndex)].body.get();

        // 骨は親が先に並んでいるので、親側の剛体の段数は既に確定している。
        const int parentDepth = bodyDepth[static_cast<std::size_t>(parentBodyIndex)];
        bodyDepth[bodyIndex]  = parentDepth + 1;

        const int child = firstChild[boneIndex];
        const math::Vector3 segment =
            bones[static_cast<std::size_t>(child)].position - bones[boneIndex].position;

        auto joint = std::make_unique<physics::XPBDJoint>(parentBody, link.body.get());
        const RagdollBoneSettings& settings = profile.Resolve(bones[boneIndex].name);
        joint->Limits() = settings.limits;
        // 関節フレームの X が骨の向き。可動域も目標姿勢もこのフレームで測る。
        joint->Build(bones[boneIndex].position,
                     AlignAxis(bones[boneIndex].rotation, math::Vector3::RIGHT, segment));

        JointLink jointLink;
        jointLink.joint      = joint.get();
        jointLink.parentBody = parentBodyIndex;
        jointLink.childBody  = static_cast<int>(bodyIndex);
        jointLink.depth      = parentDepth;
        jointLink.baseServo  = settings.servo;
        m_joints.push_back(jointLink);
        m_solver.AddConstraint(std::move(joint));
    }

    // 各関節が «その先を支える» のに要るトルクの素。実トルクは これ × |g|。
    //
    // WHY 真横の姿勢を基準にするか: バインドポーズでは脚がまっすぐ下を向いていて、
    //     重力に垂直な腕の長さがほぼ 0 になる。それを基準にすると «必要トルク 0» と
    //     出てしまうので、姿勢に依らず決まる «関節からの距離» を腕として採る。
    for (JointLink& link : m_joints) {
        const math::Vector3 anchor = link.joint->GetAnchorParentWorld();
        float moment = 0.0f;
        for (std::size_t b = 0; b < m_bodies.size(); ++b) {
            // この関節に載っているのは childBody から先の部分木だけ。
            int ancestor = static_cast<int>(b);
            while (ancestor >= 0 && ancestor != link.childBody)
                ancestor = m_bodies[static_cast<std::size_t>(ancestor)].parentBody;
            if (ancestor != link.childBody) continue;
            moment += m_bodies[b].mass * (m_bodies[b].body->GetPosition() - anchor).Length();
        }
        link.holdMoment = moment;
    }

    // 繋ぎ止めの強さの素。根から見た全身の «重さ» と «倒れにくさ»。
    for (const BodyLink& link : m_bodies) m_totalMass += link.mass;
    if (!m_anchors.empty()) {
        const math::Vector3 root =
            m_bodies[static_cast<std::size_t>(m_anchorBody.front())].body->GetPosition();
        for (const BodyLink& link : m_bodies)
            m_rootMoment += link.mass * (link.body->GetPosition() - root).Length();
    }

    // 接地はカプセルの両端で取る。中心 1 点だと寝かせた胴が床へ半分めり込む。
    //
    // WHY 関節より後に足すか: ソルバは登録順に解き、Gauss-Seidel は後に解いた方が勝つ。
    //     床を後にすると «骨がわずかに伸びてでも床から出る» になる。逆にすると
    //     骨の長さを守るために足が床へ沈み、接地が毎フレーム負ける。
    for (const BodyLink& link : m_bodies) {
        const float radius = link.collider->m_radius;
        for (const float end : { link.collider->m_halfHeight, -link.collider->m_halfHeight }) {
            auto contact = std::make_unique<physics::XPBDPlaneContact>(
                link.body.get(), math::Vector3{ 0.0f, end, 0.0f }, radius,
                math::Vector3::UP, m_groundHeight);
            contact->SetFriction(m_groundFriction);
            m_ground.push_back(contact.get());
            m_solver.AddConstraint(std::move(contact));
        }
    }

    BuildShapeInstances();

    // 組んだ時点で重なっている組は自己衝突から永久に外す。関節で繋がっていなくても
    // 肩と胸のように «元から重ねてある» 組があり、当てると起動した瞬間に押し合って自壊する。
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

    ApplyDrive();
}

// カプセル 1 個 + 両端の球 2 個を ColliderInstance にする。NarrowPhase はこの形でしか
// 受け取らないので、剛体を作った直後に一度だけ組んで、以後は姿勢だけ更新する。
void RagdollRig::BuildShapeInstances()
{
    m_instances.clear();
    m_instances.reserve(m_bodies.size() * 3);

    for (const BodyLink& link : m_bodies) {
        const float half = link.collider->m_halfHeight;

        physics::ColliderInstance capsule;
        capsule.collider = link.collider.get();
        capsule.body     = link.body.get();
        capsule.material = &m_contactSettings.surface;
        m_instances.push_back(capsule);

        for (int end = 0; end < 2; ++end) {
            physics::ColliderInstance cap;
            cap.collider     = link.caps[end].get();
            cap.body         = link.body.get();
            cap.material     = &m_contactSettings.surface;
            cap.centerOffset = { 0.0f, end == 0 ? half : -half, 0.0f };
            m_instances.push_back(cap);
        }
    }
}

void RagdollRig::ApplyDrive()
{
    const float gravity = m_solver.GetGravity().Length();

    // 繋ぎ止めもサーボと同じ «自重比» で持つ。重力を変えれば必要な力も変わる。
    const float anchorStrength = std::max(m_anchorScale, 0.0f);
    for (physics::XPBDPoseAnchor* anchor : m_anchors)
        anchor->SetStrength(m_totalMass * gravity * anchorStrength,
                            m_rootMoment * gravity * anchorStrength,
                            std::max(m_anchorSag, 0.0f), std::max(m_anchorTilt, 0.0f));

    for (JointLink& link : m_joints) {
        const float strength =
            std::max(m_driveScale, 0.0f) *
            std::pow(std::max(m_driveFalloff, 0.0f), static_cast<float>(link.depth));

        physics::XPBDJointDrive& drive = link.joint->Drive();
        const math::Quaternion target = drive.target;   // 目標は毎フレーム別に更新される

        drive         = physics::XPBDJointDrive{};
        drive.target  = target;
        drive.enabled = link.baseServo.enabled && m_driveEnabled && strength > 0.0f;
        if (!drive.enabled) continue;

        // «その先を支えるのに要るトルク» に倍率を掛けたものが上限になる。骨格の
        // 大きさが変わっても «どれだけ余裕があるか» が保たれる (RagdollServo の WHY)。
        drive.maxTorque =
            link.holdMoment * gravity * std::max(link.baseServo.torqueScale, 0.0f) * strength;
        // たわみ角 θ でのトルクは θ/α。α = holdSag / maxTorque と置くと、
        // «holdSag だけたわんだところで上限を出し切る» という一貫した意味になる。
        //
        // 弱くすると上限が下がり、同じ式で compliance が上がる ─ 硬いまま上限だけ
        // 下げると、押した瞬間は耐えて限界で急に落ちる不連続な動きになる。
        drive.compliance =
            std::max(link.baseServo.holdSag, 0.0f) / std::max(drive.maxTorque, 1.0e-4f);
        drive.damping = link.baseServo.damping * std::max(m_driveDamping, 0.0f);
    }
}

void RagdollRig::SetDrive(bool enabled, float scale, float falloff, float damping)
{
    m_driveEnabled = enabled;
    m_driveScale   = scale;
    m_driveFalloff = falloff;
    m_driveDamping = damping;
    // 脱力するときは繋ぎ止めも外す。残すと «力が抜けたのに胴だけ宙に留まる» になる。
    for (physics::XPBDPoseAnchor* anchor : m_anchors) anchor->SetEnabled(enabled);
    ApplyDrive();
}

void RagdollRig::SetRootAnchor(float scale, float sag, float tilt)
{
    m_anchorScale = scale;
    m_anchorSag   = sag;
    m_anchorTilt  = tilt;
    ApplyDrive();
}

void RagdollRig::RefreshContacts(physics::World* world)
{
    m_solver.ClearTransient();
    m_contacts.clear();
    m_contactPoints.clear();
    m_pairs.clear();
    if (m_instances.empty()) return;

    // 形状を今の姿勢へ同期する。NarrowPhase はコライダーが覚えているワールド形状を見るので、
    // ここを飛ばすと «前フレームの位置で当たり判定する» ことになる。
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

    // PhysicsSolver は状態を持たないので、呼ぶたびに作ってよい。
    physics::PhysicsSolver narrowPhase;
    narrowPhase.NarrowPhase(m_pairs, m_contactPoints);
    MakeContacts();
}

void RagdollRig::CollectWorldPairs(physics::World& world)
{
    // 全身を包む球で 1 回だけ問い合わせる。
    //
    // WHY 剛体ごとに引かないか: OverlapSphere は全コライダーを線形に走査する。20 個の骨で
    //     20 往復すると «コライダー総数 × 20» になり、アリーナ 1 枚を拾うために払う額として
    //     割に合わない。粗く 1 回拾ってから、こちらで AABB を突き合わせる方が安い。
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
                // 静的 = World が積分しないもの。地形も «剛体を持たないコライダー» なのでここ。
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
    // 自己衝突はカプセルどうしだけを見る。両端の球まで当てると同じ重なりを 9 通り
    // 報告することになり、押し戻しがそのぶん硬くなる。
    // BuildShapeInstances が «剛体 1 個につきカプセル → 端 → 端» の順で並べるので、
    // 剛体 i のカプセルは m_instances[i * 3]。
    for (std::size_t a = 0; a < m_bodies.size(); ++a) {
        for (std::size_t b = a + 1; b < m_bodies.size(); ++b) {
            if (!AllowsSelfContact(static_cast<int>(a), static_cast<int>(b))) continue;
            if (!m_bodies[a].collider->GetAABB().Overlaps(m_bodies[b].collider->GetAABB()))
                continue;
            m_pairs.push_back(physics::CollisionPair{ &m_instances[a * 3], &m_instances[b * 3] });
        }
    }
}

bool RagdollRig::AllowsSelfContact(int a, int b) const
{
    const std::size_t count = m_bodies.size();
    if (m_selfOverlapAtBuild.size() == count * count &&
        m_selfOverlapAtBuild[static_cast<std::size_t>(a) * count + static_cast<std::size_t>(b)])
        return false;

    // 関節グラフ上の距離。先祖を selfSkip 段まで遡って相手に当たれば «繋がっている» 扱い。
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
    for (const physics::ContactPoint& point : m_contactPoints) {
        if (point.isTrigger) continue;
        if (m_contacts.size() >= kMaxContacts) break;

        const int indexA = OwnBodyIndex(point.bodyA);
        const int indexB = OwnBodyIndex(point.bodyB);
        if (indexA < 0 && indexB < 0) continue;

        // 法線は B → A。押し出される側をこちらに揃えると、拘束は片側だけを見ればよくなる。
        physics::RigidBody* body   = indexA >= 0 ? point.bodyA : point.bodyB;
        physics::RigidBody* other  = indexA >= 0 ? point.bodyB : point.bodyA;
        const math::Vector3 normal = indexA >= 0 ? point.normal : -point.normal;
        // 相手も自分の骨なら双方を動かす。World が積分している剛体は動かさない
        // (同じフレームで 2 回進んでしまう。反作用は ApplyContactReactions で返す)。
        const bool solveOther = indexA >= 0 && indexB >= 0;

        const bool hasMaterials = point.materialA && point.materialB;
        const float friction = hasMaterials
            ? physics::PhysicsMaterial::CombineFriction(*point.materialA, *point.materialB)
            : m_contactSettings.surface.dynamicFriction;
        const float restitution = hasMaterials
            ? physics::PhysicsMaterial::CombineRestitution(*point.materialA, *point.materialB)
            : m_contactSettings.surface.restitution;

        m_contacts.emplace_back();
        m_contacts.back().Set(body, other, solveOther, point.point, normal,
                              point.depth, friction, restitution);
    }

    // 実体が出揃ってからソルバへ渡す。作りながら渡すと、再確保で全部が宙を指す。
    for (physics::XPBDContact& contact : m_contacts) m_solver.AddTransient(&contact);
}

void RagdollRig::ApplyContactReactions()
{
    for (physics::XPBDContact& contact : m_contacts) {
        physics::RigidBody* other = contact.GetOther();
        if (!other || contact.IsOtherSolved() || other->IsStatic()) continue;

        const math::Vector3 impulse = contact.ReactionImpulse();
        if (impulse.LengthSq() <= 1.0e-8f) continue;
        other->ApplyImpulseAtPoint(impulse, contact.ContactPointWorld());
    }
}

void RagdollRig::SetGround(float height, float friction)
{
    m_groundHeight   = height;
    m_groundFriction = friction;
    for (physics::XPBDPlaneContact* contact : m_ground) {
        contact->SetPlane(math::Vector3::UP, height);
        contact->SetFriction(friction);
    }
}

void RagdollRig::DisableGround()
{
    SetGround(kNoGround, 0.0f);
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

        const float radius = link.collider->m_radius;
        for (const float end : { link.collider->m_halfHeight, -link.collider->m_halfHeight }) {
            const float y =
                (position + rotation * math::Vector3{ 0.0f, end, 0.0f }).y - radius;
            if (!found || y < lowest) { lowest = y; found = true; }
        }
    }
    return lowest;
}

void RagdollRig::ApplyImpulse(const math::Vector3& origin,
                              const math::Vector3& velocity,
                              float                radius)
{
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

    // 剛体を «瞬間移動» させたので、接触が覚えている前フレームの位置は無効。
    for (physics::XPBDPlaneContact* contact : m_ground) contact->ResetHistory();
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

    // 根は関節を持たないので、繋ぎ止めの目標をここで取り直す。これが無いと
    // サーボが形を保ったまま全体が落ちていく。
    for (std::size_t i = 0; i < m_anchors.size(); ++i) {
        const int bodyIndex = m_anchorBody[i];
        const int boneIndex = m_bodies[static_cast<std::size_t>(bodyIndex)].boneIndex;
        if (boneIndex < 0 || boneIndex >= static_cast<int>(bones.size())) continue;

        math::Vector3    position;
        math::Quaternion rotation;
        BodyPoseFromBone(bodyIndex, bones[static_cast<std::size_t>(boneIndex)],
                         position, rotation);
        m_anchors[i]->SetTarget(position, rotation);
    }

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
        const math::Quaternion target = (parentFrame.Inverse() * childFrame).Normalized();
        link.joint->Drive().target = target;

        if (!m_learnLimits) continue;
        physics::XPBDJointLimits& limits = link.joint->Limits();
        if (!limits.enabled) continue;

        // 目標そのものを swing / twist へ分解すれば «クリップがこの関節に要求している
        // 角度» が出る。可動域と同じ «たわみ 0 からの量» なので、そのまま比べられる。
        math::Quaternion swing;
        math::Quaternion twist;
        physics::DecomposeSwingTwist(target, swing, twist);
        const math::Vector3 swingVector = physics::RotationVector(swing);
        // twist は X 軸まわりだけなので、回転ベクトルの X 成分がそのまま角になる。
        const float twistAngle = physics::RotationVector(twist).x;

        limits.twistMin  = std::min(limits.twistMin,  twistAngle - m_limitMargin);
        limits.twistMax  = std::max(limits.twistMax,  twistAngle + m_limitMargin);
        limits.swingMinY = std::min(limits.swingMinY, swingVector.y - m_limitMargin);
        limits.swingMaxY = std::max(limits.swingMaxY, swingVector.y + m_limitMargin);
        limits.swingMinZ = std::min(limits.swingMinZ, swingVector.z - m_limitMargin);
        limits.swingMaxZ = std::max(limits.swingMaxZ, swingVector.z + m_limitMargin);
    }
}

void RagdollRig::SetLimitLearning(bool enabled, float margin)
{
    m_learnLimits = enabled;
    m_limitMargin = std::max(margin, 0.0f);
}

int RagdollRig::CountLimitedJoints() const
{
    int count = 0;
    for (const JointLink& link : m_joints)
        if (link.joint->IsLimited()) ++count;
    return count;
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

void RagdollRig::BuildDebugLines(std::vector<RagdollDebugLine>& out) const
{
    using Kind = RagdollDebugLine::Kind;
    const auto push = [&out](const math::Vector3& from, const math::Vector3& to, Kind kind) {
        out.push_back(RagdollDebugLine{ from, to, kind });
    };

    for (const BodyLink& link : m_bodies) {
        const physics::ColliderDebugGeometry geometry =
            physics::BuildColliderDebugGeometry(*link.collider);
        for (const physics::DebugLine& line : geometry.lines)
            push(line.from, line.to, Kind::Body);
    }

    for (const JointLink& link : m_joints) {
        const physics::XPBDJoint&  joint  = *link.joint;
        const math::Vector3        anchor = joint.GetAnchorParentWorld();
        const math::Quaternion     frame  = joint.GetParentFrameWorld();
        const physics::XPBDJointLimits& limits = joint.Limits();
        const Kind kind = joint.IsDriveSaturated() ? Kind::JointSaturated : Kind::Joint;

        // 骨の長さに合わせて錐の大きさを決める。固定長にすると、大きい骨では潰れ、
        // 小さい骨では錐だけが目立って «どの関節の可動域か» が読めなくなる。
        const float length =
            std::max(m_bodies[static_cast<std::size_t>(link.childBody)].collider->m_halfHeight,
                     0.05f);

        // 中立軸 (関節フレームの X = 骨の向き)。今どこを向いているかは子フレームで出す。
        push(anchor, anchor + frame * math::Vector3::RIGHT * length, kind);
        push(anchor,
             anchor + joint.GetChildFrameWorld() * math::Vector3::RIGHT * (length * 1.2f),
             kind);
        if (!limits.enabled) continue;

        // 可動域の «角»。Y/Z それぞれの上下限まで中立軸を倒し、4 本を四角で結ぶ。
        // 円錐ではなく角錐なのは、可動域そのものが軸ごとの上下限で書かれているため。
        math::Vector3 corners[4];
        const float swings[4][2] = {
            { limits.swingMinY, limits.swingMinZ }, { limits.swingMaxY, limits.swingMinZ },
            { limits.swingMaxY, limits.swingMaxZ }, { limits.swingMinY, limits.swingMaxZ },
        };
        for (int i = 0; i < 4; ++i) {
            const math::Quaternion swing =
                math::Quaternion::FromAxisAngle(math::Vector3::UP, swings[i][0]) *
                math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, swings[i][1]);
            corners[i] = anchor + (frame * swing) * math::Vector3::RIGHT * length;
        }
        for (int i = 0; i < 4; ++i) {
            push(anchor, corners[i], kind);
            push(corners[i], corners[(i + 1) % 4], kind);
        }
    }

    for (const physics::XPBDContact& contact : m_contacts) {
        if (contact.GetPenetration() <= 0.0f) continue;
        const math::Vector3 point = contact.ContactPointWorld();
        push(point, point + contact.GetNormal() * 0.25f, Kind::Contact);
    }
}

int RagdollRig::CountSaturatedJoints() const
{
    int count = 0;
    for (const JointLink& link : m_joints)
        if (link.joint->IsDriveSaturated()) ++count;
    return count;
}

} // namespace fbzz::scene
