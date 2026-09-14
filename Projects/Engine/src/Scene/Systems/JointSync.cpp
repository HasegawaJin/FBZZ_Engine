/// @file    JointSync.cpp
/// @brief   JointComponent → physics::Constraint の張り直しと解除。
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include "Engine/Scene/Systems/JointSync.hpp"

#include "Engine/Scene/Components/JointComponent.hpp"
#include "Engine/Scene/Components/RigidBodyComponent.hpp"
#include "Engine/Scene/GameObject.hpp"
#include "Engine/Scene/Scene.hpp"

#include <Engine/Profiler/ProfileScope.hpp>
#include <Math/MathUtils.hpp>
#include <Physics/World.hpp>

#include <memory>
#include <utility>
#include <vector>

namespace fbzz::scene {

namespace {

// PhysicsSystem::SyncRigidBodies と同じ有効条件。
// WHY そろえるか: ここだけ条件が緩いと «World が積分していない剛体» を制約が動かす。
//     見た目は «無効にしたのに動く» で、原因が物理側にあるとは読めない壊れ方になる。
physics::RigidBody* SimulatedJointBody(GameObject* go)
{
    if (!go || !go->activeInHierarchy()) return nullptr;
    auto* rb = go->GetComponent<RigidBodyComponent>();
    if (!rb || !rb->enabled) return nullptr;
    return rb->rigidBody.get();
}

// connectedBody 未指定のときの相手探し。祖先を根へ向かってたどる。
//
// WHY 祖先か: 吊り下げ物・鎖の節は必ず «吊り元の下» に置かれる。SocketAttachment が
//     socketName を祖先方向へ探すのと同じ理由で、この探索で一意が取れる。
physics::RigidBody* FindAncestorJointBody(GameObject& go)
{
    for (GameObject* parent = go.GetParent(); parent; parent = parent->GetParent())
        if (physics::RigidBody* body = SimulatedJointBody(parent)) return body;
    return nullptr;
}

physics::ConstraintType ToPhysicsConstraintType(JointType type)
{
    switch (type) {
    case JointType::Distance: return physics::ConstraintType::DISTANCE;
    case JointType::Rope:     return physics::ConstraintType::ROPE;
    case JointType::Spring:   return physics::ConstraintType::SPRING;
    case JointType::Hinge:    return physics::ConstraintType::HINGE;
    case JointType::Slider:   return physics::ConstraintType::SLIDER;
    case JointType::Chain:    return physics::ConstraintType::CHAIN;
    case JointType::Fixed:    break;
    }
    return physics::ConstraintType::FIXED;
}

void ReleaseJoint(physics::World& world, JointComponent& joint)
{
    if (joint.constraintHandle.IsValid()) world.RemoveConstraint(joint.constraintHandle);
    joint.constraintHandle = {};
    joint.connected = false;
}

float GapBetweenBodies(const physics::RigidBody& a, const physics::RigidBody& b)
{
    return (b.GetPosition() - a.GetPosition()).Length();
}

// 長さ 0 の軸は上向きへ倒す。
// WHY 素の Normalized() を通さないか: 正規化の契約は長さ 0 を «通報して (0,0,0) を返す»。
//     ここは毎フレーム通る経路なので、軸を空にした瞬間からログが埋まる。
math::Vector3 SafeJointAxis(const math::Vector3& axis)
{
    return axis.LengthSq() > 1e-8f ? axis.Normalized() : math::Vector3::UP;
}

// 種別ごとの «今の値» を生きている制約へ書き込む。
// WHY 作り直さず書くか: Fixed / Hinge は構築時の相対姿勢を基準として抱えている。
//     毎フレーム作り直すと基準が «今の姿勢» へ更新され続け、溶接も可動域も効かなくなる。
void ApplyJointTunables(physics::Constraint& constraint, const JointComponent& joint)
{
    switch (joint.type) {
    case JointType::Distance:
        static_cast<physics::DistanceConstraint&>(constraint).m_distance = joint.resolvedDistance;
        break;
    case JointType::Rope:
        static_cast<physics::RopeConstraint&>(constraint).m_maxLength = joint.resolvedDistance;
        break;
    case JointType::Spring: {
        auto& spring = static_cast<physics::SpringConstraint&>(constraint);
        spring.m_restLength = joint.resolvedDistance;
        spring.m_stiffness  = joint.spring;
        spring.m_damping    = joint.damping;
        break;
    }
    case JointType::Hinge: {
        auto& hinge = static_cast<physics::HingeConstraint&>(constraint);
        hinge.m_localAnchorA = joint.anchor;
        hinge.m_localAnchorB = joint.connectedAnchor;
        if (joint.useLimits)
            hinge.SetLimits(math::ToRad(joint.lowerLimit), math::ToRad(joint.upperLimit));
        else
            hinge.ClearLimits();
        if (joint.useMotor)
            hinge.SetMotor(math::ToRad(joint.motorSpeed), joint.motorMaxTorque);
        else
            hinge.ClearMotor();
        break;
    }
    case JointType::Slider: {
        auto& slider = static_cast<physics::SliderConstraint&>(constraint);
        slider.m_axis = SafeJointAxis(joint.axis);
        if (joint.useLimits)
            slider.SetLimits(joint.lowerLimit, joint.upperLimit);
        else
            slider.ClearLimits();
        break;
    }
    case JointType::Chain: {
        auto& chain = static_cast<physics::ChainConstraint&>(constraint);
        chain.m_segmentLength    = joint.resolvedDistance;
        chain.m_solverIterations = joint.solverIterations < 1 ? 1 : joint.solverIterations;
        break;
    }
    case JointType::Fixed:
        break;
    }
}

// 生きている制約が今の構成のままか。false なら作り直す。
bool JointStillMatches(const physics::Constraint& constraint,
             const JointComponent& joint,
             const std::vector<physics::RigidBody*>& bodies)
{
    if (constraint.GetType() != ToPhysicsConstraintType(joint.type)) return false;

    if (joint.type == JointType::Chain) {
        const auto& current = static_cast<const physics::ChainConstraint&>(constraint).GetBodies();
        return current == bodies;
    }

    if (bodies.size() != 2u) return false;
    if (constraint.GetBodyA() != bodies[0] || constraint.GetBodyB() != bodies[1]) return false;

    // Hinge は構築時に軸の直交基準を焼き込む。軸だけ書き換えると角度の測り方が
    // 古い基準のまま残り、可動域が «違う向きで» 効く。軸が変わったら作り直す。
    if (joint.type == JointType::Hinge) {
        const auto& hinge = static_cast<const physics::HingeConstraint&>(constraint);
        const math::Vector3 wanted = SafeJointAxis(joint.axis);
        if ((hinge.m_axis - wanted).LengthSq() > 1e-6f) return false;
    }
    return true;
}

std::unique_ptr<physics::Constraint> BuildJointConstraint(
    const JointComponent& joint,
    const std::vector<physics::RigidBody*>& bodies)
{
    if (joint.type == JointType::Chain)
        return std::make_unique<physics::ChainConstraint>(
            bodies, joint.resolvedDistance,
            joint.solverIterations < 1 ? 1 : joint.solverIterations);

    if (bodies.size() != 2u) return nullptr;
    physics::RigidBody* a = bodies[0];
    physics::RigidBody* b = bodies[1];

    switch (joint.type) {
    case JointType::Distance:
        return std::make_unique<physics::DistanceConstraint>(a, b, joint.resolvedDistance);
    case JointType::Rope:
        return std::make_unique<physics::RopeConstraint>(a, b, joint.resolvedDistance);
    case JointType::Spring:
        return std::make_unique<physics::SpringConstraint>(
            a, b, joint.resolvedDistance, joint.spring, joint.damping);
    case JointType::Hinge:
        return std::make_unique<physics::HingeConstraint>(
            a, b, joint.anchor, joint.connectedAnchor, SafeJointAxis(joint.axis));
    case JointType::Slider:
        return std::make_unique<physics::SliderConstraint>(a, b, SafeJointAxis(joint.axis));
    case JointType::Fixed:
    case JointType::Chain:
        break;
    }
    return std::make_unique<physics::FixedConstraint>(a, b);
}

// 制約に渡す剛体を並べる。ペア関節は [自分, 相手]、Chain は [自分, 2 節目, ...]。
// 1 つでも欠けたら空を返す (= 張らない)。
std::vector<physics::RigidBody*> CollectJointBodies(Scene& scene,
                                               GameObject& go,
                                               const JointComponent& joint,
                                               physics::RigidBody* self)
{
    std::vector<physics::RigidBody*> bodies;

    if (joint.type == JointType::Chain) {
        bodies.reserve(joint.chainBodies.size() + 1u);
        bodies.push_back(self);
        for (const EntityRef& ref : joint.chainBodies) {
            physics::RigidBody* body = SimulatedJointBody(ref.Resolve(scene));
            if (!body) return {};
            bodies.push_back(body);
        }
        if (bodies.size() < 2u) return {};
        return bodies;
    }

    physics::RigidBody* other = ResolveJointPartner(scene, go, joint);

    // 自分自身を相手にすると invMass の和で 0 除算を踏む手前まで行って、
    // «動かないのに CPU だけ食う» 制約になる。黙って張らない方が読み解ける。
    if (!other || other == self) return {};

    bodies.push_back(self);
    bodies.push_back(other);
    return bodies;
}

void SyncOneJoint(Scene& scene, GameObject& go, JointComponent& joint, physics::World& world)
{
    physics::RigidBody* self = SimulatedJointBody(&go);
    if (!self) {
        ReleaseJoint(world, joint);
        return;
    }

    const std::vector<physics::RigidBody*> bodies = CollectJointBodies(scene, go, joint, self);
    if (bodies.empty()) {
        ReleaseJoint(world, joint);
        return;
    }

    physics::Constraint* live = world.FindConstraint(joint.constraintHandle);
    const bool reusable = live && JointStillMatches(*live, joint, bodies);

    if (!reusable) {
        // 距離は «張った瞬間の間隔» を採る。以降は resolvedDistance を正として扱うので、
        // 毎フレーム測り直して距離がじわじわ伸びていく (制約が効かない) ことはない。
        joint.resolvedDistance = joint.distance;
        if (joint.autoDistance && joint.UsesDistance())
            joint.resolvedDistance = GapBetweenBodies(*bodies[0], *bodies[1]);

        std::unique_ptr<physics::Constraint> built = BuildJointConstraint(joint, bodies);
        if (!built) {
            ReleaseJoint(world, joint);
            return;
        }
        ApplyJointTunables(*built, joint);
        joint.constraintHandle = world.SyncConstraint(joint.constraintHandle, std::move(built));
        joint.connected = joint.constraintHandle.IsValid();
        return;
    }

    if (!world.KeepConstraint(joint.constraintHandle)) {
        ReleaseJoint(world, joint);
        return;
    }
    if (!joint.autoDistance) joint.resolvedDistance = joint.distance;
    ApplyJointTunables(*live, joint);
    joint.connected = true;
}

} // namespace

physics::RigidBody* ResolveJointPartner(Scene& scene,
                                        GameObject& go,
                                        const JointComponent& joint)
{
    if (joint.type == JointType::Chain) return nullptr;
    if (joint.connectedBody.IsValid())
        return SimulatedJointBody(joint.connectedBody.Resolve(scene));
    return joint.connectToParent ? FindAncestorJointBody(go) : nullptr;
}

void SyncJointComponents(Scene& scene, physics::World& world)
{
    FBZZ_PROFILE_SCOPE("PhysicsSystem::SyncJoints");

    for (EntityID id : scene.GetEntities<JointComponent>()) {
        auto* joint = scene.GetComponent<JointComponent>(id);
        if (!joint) continue;

        GameObject* go = scene.GetGameObject(id);
        if (!go || !go->activeInHierarchy() || !joint->enabled) {
            ReleaseJoint(world, *joint);
            continue;
        }
        SyncOneJoint(scene, *go, *joint, world);
    }
}

} // namespace fbzz::scene
