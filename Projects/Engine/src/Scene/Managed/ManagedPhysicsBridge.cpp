/// @file    ManagedPhysicsBridge.cpp
/// @brief   剛体のコピー検証と Scene に属する Collider の安全な検索。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include "ManagedPhysicsBridge.hpp"
#include "ManagedServicesBridge.hpp"
#include "Engine/Scene/Components/ColliderComponent.hpp"
#include "Engine/Scene/Components/RigidBodyComponent.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Systems/TransformSystem.hpp"
#include "Math/MathUtils.hpp"
#include "Physics/World.hpp"
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Scene/ScriptProxy/ScriptColliderProxy.hpp>
#include <algorithm>
#include <cmath>
#include <charconv>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::scene::managed_detail {
namespace {

static_assert(sizeof(math::Vector3) == 12);

bool Finite(const math::Vector3& value)
{ return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }

bool Finite(const math::Quaternion& value)
{ return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w); }

bool Normalizable(const math::Quaternion& value)
{ const float length = value.Length(); return Finite(value) && std::isfinite(length) && !math::NearlyZero(length); }

bool Invertible(const math::Quaternion& value)
{
    const float normSq = value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
    return Finite(value) && std::isfinite(normSq) && !math::NearlyZero(normSq);
}

bool Finite(const Transform& value)
{
    return Finite(value.position) && Finite(value.rotation) && Finite(value.scale)
        && Finite(value.worldPosition) && Finite(value.worldRotation) && Finite(value.worldScale);
}

bool Near(float actual, float expected)
{ return std::abs(actual - expected) <= math::EPSILON * std::max(1.0f, std::abs(expected)); }

bool Near(const math::Vector3& actual, const math::Vector3& expected)
{ return Near(actual.x, expected.x) && Near(actual.y, expected.y) && Near(actual.z, expected.z); }

math::Vector3 Vector(const ManagedBridgeRequest& request) { return {request.x, request.y, request.z}; }

math::Vector3 InputVector(const ManagedBridgeRequest& request)
{
    math::Vector3 result;
    std::memcpy(&result, request.input, sizeof(result));
    return result;
}

ManagedBridgeStatus ReplyVector(ManagedBridgeReply& reply, const math::Vector3& value)
{
    if (!Finite(value)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    reply.valueKind = ManagedValueKind::VECTOR3;
    reply.x = value.x; reply.y = value.y; reply.z = value.z;
    return ManagedBridgeStatus::OK;
}

ManagedBridgeStatus ReplyScalar(ManagedBridgeReply& reply, float value)
{
    if (!std::isfinite(value)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    reply.valueKind = ManagedValueKind::FLOAT; reply.x = value;
    return ManagedBridgeStatus::OK;
}

ManagedBridgeStatus ReplyFlag(ManagedBridgeReply& reply, bool value)
{
    reply.valueKind = ManagedValueKind::BOOL; reply.integer = value ? 1 : 0;
    return ManagedBridgeStatus::OK;
}

bool ApplyWrite(physics::RigidBody& body, const ManagedBridgeRequest& request)
{
    switch (static_cast<ManagedPhysicsBodyValue>(request.index)) {
    case ManagedPhysicsBodyValue::VELOCITY:
        if (!Finite(Vector(request))) return false;
        body.SetVelocity(Vector(request)); return true;
    case ManagedPhysicsBodyValue::ANGULAR_VELOCITY:
        if (!Finite(Vector(request))) return false;
        body.SetAngularVelocity(Vector(request)); return true;
    case ManagedPhysicsBodyValue::MASS:
        if (!std::isfinite(request.x) || request.x < 0.0f
            || (request.x > 0.0f && !std::isfinite(1.0f / request.x))) return false;
        body.SetMass(request.x); return true;
    case ManagedPhysicsBodyValue::GRAVITY_SCALE:
        if (!std::isfinite(request.x)) return false;
        body.m_gravityScale = request.x; return true;
    case ManagedPhysicsBodyValue::IS_STATIC:
        if (request.x != 0.0f && request.x != 1.0f) return false;
        body.m_isStatic = request.x == 1.0f; body.SetMass(body.GetMass()); return true;
    case ManagedPhysicsBodyValue::LINEAR_DRAG:
    case ManagedPhysicsBodyValue::ANGULAR_DRAG:
    case ManagedPhysicsBodyValue::CCD_RADIUS:
    case ManagedPhysicsBodyValue::FLOW_COUPLING:
        if (!std::isfinite(request.x) || request.x < 0.0f) return false;
        if (request.index == static_cast<uint32_t>(ManagedPhysicsBodyValue::LINEAR_DRAG)) body.m_linearDrag = request.x;
        else if (request.index == static_cast<uint32_t>(ManagedPhysicsBodyValue::ANGULAR_DRAG)) body.m_angularDrag = request.x;
        else if (request.index == static_cast<uint32_t>(ManagedPhysicsBodyValue::CCD_RADIUS)) body.m_ccdRadius = request.x;
        else body.SetFlowCoupling(request.x);
        return true;
    case ManagedPhysicsBodyValue::LOCAL_TIME_SCALE:
        if (!std::isfinite(request.x) || request.x < 0.0f || request.x > 8.0f) return false;
        body.m_timeScale = request.x; return true;
    case ManagedPhysicsBodyValue::USE_CCD:
    case ManagedPhysicsBodyValue::USE_GRAVITY:
    case ManagedPhysicsBodyValue::ALLOW_SLEEPING:
        if (request.x != 0.0f && request.x != 1.0f) return false;
        if (request.index == static_cast<uint32_t>(ManagedPhysicsBodyValue::USE_CCD)) body.m_useCCD = request.x == 1.0f;
        else if (request.index == static_cast<uint32_t>(ManagedPhysicsBodyValue::USE_GRAVITY)) body.m_useGravity = request.x == 1.0f;
        else { body.m_allowSleeping = request.x == 1.0f; if (!body.m_allowSleeping) body.WakeUp(); }
        return true;
    default: return false;
    }
}

bool ApplyCommand(physics::RigidBody& body, const ManagedBridgeRequest& request)
{
    if (!Finite(Vector(request))) return false;
    const physics::AxisLock axes{(request.flags & 1u) != 0, (request.flags & 2u) != 0, (request.flags & 4u) != 0};
    switch (static_cast<ManagedPhysicsBodyCommand>(request.index)) {
    case ManagedPhysicsBodyCommand::FORCE: body.ApplyForce(Vector(request)); return true;
    case ManagedPhysicsBodyCommand::IMPULSE: body.ApplyImpulse(Vector(request)); return true;
    case ManagedPhysicsBodyCommand::TORQUE: body.ApplyTorque(Vector(request)); return true;
    case ManagedPhysicsBodyCommand::ANGULAR_IMPULSE: body.ApplyAngularImpulse(Vector(request)); return true;
    case ManagedPhysicsBodyCommand::FORCE_AT_POINT: {
        const math::Vector3 point = InputVector(request);
        if (!Finite(point)) return false;
        body.ApplyForceAtPoint(Vector(request), point); return true;
    }
    case ManagedPhysicsBodyCommand::POINT_IMPULSE: {
        const math::Vector3 point = InputVector(request);
        if (!Finite(point)) return false;
        body.ApplyImpulseAtPoint(Vector(request), point); return true;
    }
    case ManagedPhysicsBodyCommand::WAKE: body.WakeUp(); return true;
    case ManagedPhysicsBodyCommand::SLEEP: body.Sleep(); return true;
    case ManagedPhysicsBodyCommand::FREEZE_POSITION: body.SetFreezePosition(axes); return true;
    case ManagedPhysicsBodyCommand::FREEZE_ROTATION: body.SetFreezeRotation(axes); return true;
    case ManagedPhysicsBodyCommand::TELEPORT: body.SetPosition(Vector(request)); return true;
    default: return false;
    }
}

using ColliderOwners = std::unordered_map<const physics::Collider*, GameObject*>;

/// @note Compare the borrowed address with current owned shapes before reading the source or returning an owner.
template<typename T>
GameObject* FindColliderOwner(Scene& scene, const physics::Collider* source)
{
    for (EntityID id : scene.GetEntities<T>()) {
        const T* component = scene.GetComponent<T>(id);
        if (!component || !component->enabled || component->collider.get() != source) continue;
        GameObject* object = scene.GetGameObject(id);
        if (object && object->activeInHierarchy()) return object;
    }
    return nullptr;
}
GameObject* FindQueryColliderOwner(Scene& scene, const physics::Collider* source)
{
    if (auto* object = FindColliderOwner<AabbColliderComponent>(scene,source)) return object;
    if (auto* object = FindColliderOwner<BoxColliderComponent>(scene,source)) return object;
    if (auto* object = FindColliderOwner<SphereColliderComponent>(scene,source)) return object;
    if (auto* object = FindColliderOwner<CapsuleColliderComponent>(scene,source)) return object;
    if (auto* object = FindColliderOwner<CylinderColliderComponent>(scene,source)) return object;
    if (auto* object = FindColliderOwner<MeshColliderComponent>(scene,source)) return object;
    if (auto* object = FindColliderOwner<ConvexHullColliderComponent>(scene,source)) return object;
    return FindColliderOwner<TerrainColliderComponent>(scene,source);
}
template<typename T>
void CollectColliderOwners(Scene& scene, ColliderOwners& owners)
{
    for (EntityID id : scene.GetEntities<T>()) {
        const T* component = scene.GetComponent<T>(id);
        if (!component || !component->enabled || !component->collider) continue;
        GameObject* object = scene.GetGameObject(id);
        if (object && object->activeInHierarchy()) owners.emplace(component->collider.get(),object);
    }
}
/// @note Broad All/Overlap queries and unusually many ray candidates use O(component count) collection, never Scene GameObjects.
void CollectQueryColliderOwners(Scene& scene, ColliderOwners& owners)
{
    const profiler::ProfileScope ownersBuildScope(profiler::ProfilerMarker{"ManagedPhysicsBridge::Query.OwnersBuild", "PhysicsQuery"});
    CollectColliderOwners<AabbColliderComponent>(scene,owners); CollectColliderOwners<BoxColliderComponent>(scene,owners);
    CollectColliderOwners<SphereColliderComponent>(scene,owners); CollectColliderOwners<CapsuleColliderComponent>(scene,owners);
    CollectColliderOwners<CylinderColliderComponent>(scene,owners); CollectColliderOwners<MeshColliderComponent>(scene,owners);
    CollectColliderOwners<ConvexHullColliderComponent>(scene,owners); CollectColliderOwners<TerrainColliderComponent>(scene,owners);
}

template<typename T>
bool OwnsCollider(GameObject& object, const physics::Collider* source)
{
    const T* component = object.GetComponent<T>();
    return component && component->collider.get() == source;
}

/// @note Scene による所有確認は Engine 側で行う。比較が終わるまで借用 pointer の参照先を読まない。
const physics::Collider* LiveInertiaSource(GameObject& target, const physics::Collider* source)
{
    if (!source || !target.GetScene()) return nullptr;
    for (GameObject& object : target.GetScene()->GameObjects())
        if (OwnsCollider<AabbColliderComponent>(object, source) || OwnsCollider<BoxColliderComponent>(object, source)
            || OwnsCollider<SphereColliderComponent>(object, source) || OwnsCollider<CapsuleColliderComponent>(object, source)
            || OwnsCollider<CylinderColliderComponent>(object, source) || OwnsCollider<MeshColliderComponent>(object, source)
            || OwnsCollider<ConvexHullColliderComponent>(object, source) || OwnsCollider<TerrainColliderComponent>(object, source)) return source;
    return nullptr;
}

/// @note 形状を再生成する前に、同じ Scene の剛体が借用中の旧形状を手放す。次回同期で新形状が設定される。
void ReleaseInertiaSource(GameObject& target, const physics::Collider* source)
{
    if (!source || !target.GetScene()) return;
    for (GameObject& object : target.GetScene()->GameObjects())
        if (auto* component = object.GetComponent<RigidBodyComponent>(); component && component->rigidBody
            && component->rigidBody->GetInertiaCollider() == source) component->rigidBody->SetInertiaFromCollider(nullptr);
}

ColliderComponent* SelectCollider(GameObject& target, uint32_t kind)
{
    switch (kind) {
    case 2: return target.GetComponent<BoxColliderComponent>();
    case 3: return target.GetComponent<SphereColliderComponent>();
    case 4: return target.GetComponent<CapsuleColliderComponent>();
    case 15: return target.GetComponent<CylinderColliderComponent>();
    case 16: return target.GetComponent<AabbColliderComponent>();
    case 17: return target.GetComponent<MeshColliderComponent>();
    case 18: return target.GetComponent<ConvexHullColliderComponent>();
    case 19: return target.GetComponent<TerrainColliderComponent>();
    default: return nullptr;
    }
}

bool ShapeBoundsFinite(const GameObject& target, const math::Vector3& extent)
{
    const auto& pose = target.transform;
    const math::Vector3 scaled{extent.x * std::abs(pose.worldScale.x), extent.y * std::abs(pose.worldScale.y), extent.z * std::abs(pose.worldScale.z)};
    const float radius = scaled.Length();
    return Finite(pose) && Normalizable(pose.worldRotation) && Finite(extent) && std::isfinite(extent.LengthSq())
        && Finite(scaled) && std::isfinite(radius) && Finite(pose.worldPosition + math::Vector3{radius, radius, radius})
        && Finite(pose.worldPosition - math::Vector3{radius, radius, radius});
}

} /// @note namespace

Transform* ManagedPhysicsBridge::Shadow(GameObject& target)
{
    const uint32_t index = target.GetID().index;
    if (index >= m_shadow.size()) return nullptr;
    if (m_epochs[index] != m_epoch) { m_shadow[index] = target.transform; m_epochs[index] = m_epoch; }
    return &m_shadow[index];
}

bool ManagedPhysicsBridge::Update(GameObject& target)
{
    Transform* value = Shadow(target);
    GameObject* parent = target.GetParent();
    const Transform* parentValue = parent ? Shadow(*parent) : nullptr;
    if (!value || (parent && !parentValue) || !Finite(value->position)
        || !Finite(value->scale) || !Normalizable(value->rotation)) return false;
    if (parentValue && (!Finite(parentValue->worldPosition) || !Finite(parentValue->worldScale)
        || !Normalizable(parentValue->worldRotation) || !Normalizable(parentValue->worldRotation * value->rotation))) return false;
    UpdateWorldTransform(*value, parentValue);
    return Finite(*value);
}

bool ManagedPhysicsBridge::Synchronize(GameObject& target)
{
    Scene* scene = target.GetScene();
    if (!scene) return false;
    const size_t slots = scene->EntitySlotCount();
    m_ancestors.clear();
    for (GameObject* parent = target.GetParent(); parent; parent = parent->GetParent()) {
        if (m_ancestors.size() >= slots || parent->GetScene() != scene
            || scene->GetGameObject(parent->GetID()) != parent) return false;
        m_ancestors.push_back(parent);
    }
    for (auto it = m_ancestors.rbegin(); it != m_ancestors.rend(); ++it)
        if (!Update(**it)) return false;
    m_queue.clear(); m_queue.push_back(&target);
    for (size_t index = 0; index < m_queue.size(); ++index) {
        GameObject& object = *m_queue[index];
        if (object.GetScene() != scene || scene->GetGameObject(object.GetID()) != &object || !Update(object)) return false;
        for (int child = 0; child < object.GetChildCount(); ++child)
            if (GameObject* value = object.GetChild(child)) {
                if (m_queue.size() >= slots || value->GetParent() != &object) return false;
                m_queue.push_back(value);
            }
    }
    return true;
}

bool ManagedPhysicsBridge::PrepareTeleport(GameObject& target, const math::Vector3& position, const math::Quaternion* rotation)
{
    if (!target.GetScene()) return false;
    const size_t slots = target.GetScene()->EntitySlotCount();
    if (m_shadow.size() != slots) {
        m_shadow.resize(slots); m_epochs.resize(slots);
    }
    if (++m_epoch == 0) { std::fill(m_epochs.begin(), m_epochs.end(), 0); m_epoch = 1; }
    if (!Synchronize(target)) return false;
    Transform* value = Shadow(target);
    const math::Quaternion localRotation = value->rotation;
    const math::Vector3 localScale = value->scale;
    GameObject* parent = target.GetParent();
    const Transform* parentValue = parent ? Shadow(*parent) : nullptr;
    if (parentValue) {
        if (!Invertible(parentValue->worldRotation)) return false;
        const math::Quaternion inverse = parentValue->worldRotation.Inverse();
        const math::Vector3 offset = position - parentValue->worldPosition;
        const math::Vector3 localOffset = inverse * offset;
        if (!Finite(inverse) || !Normalizable(inverse * (rotation ? *rotation : value->worldRotation))
            || !Finite(offset) || !Finite(localOffset)) return false;
        /// @note 零スケールの表現可否は親の原点から測る。大きな絶対座標で許容誤差を拡大しない。
        if ((std::abs(parentValue->worldScale.x) <= math::EPSILON && std::abs(localOffset.x) > math::EPSILON)
            || (std::abs(parentValue->worldScale.y) <= math::EPSILON && std::abs(localOffset.y) > math::EPSILON)
            || (std::abs(parentValue->worldScale.z) <= math::EPSILON && std::abs(localOffset.z) > math::EPSILON)) return false;
    }
    SetWorldPose(*value, parentValue, position, rotation ? *rotation : value->worldRotation, value->worldScale);
    if (!Finite(*value)) return false;
    if (!rotation) value->rotation = localRotation;
    value->scale = localScale;
    if (!Synchronize(target) || !Near(value->worldPosition, position)) return false;
    /// @note 子孫の動的剛体は次の既存 PhysicsSystem 同期で外部姿勢変更として処理する。
    return Normalizable(value->worldRotation);
}

void ManagedPhysicsBridge::CommitTransforms()
{
    for (GameObject* object : m_ancestors) object->transform = *Shadow(*object);
    for (GameObject* object : m_queue) object->transform = *Shadow(*object);
}

ManagedBridgeStatus ManagedPhysicsBridge::RunBody(GameObject& target, const ManagedBridgeRequest& request,
    ManagedBridgeReply& reply, bool fixedUpdate)
{
    const bool reading = request.op == ManagedBridgeOp::PHYSICS_BODY_READ;
    const bool writing = request.op == ManagedBridgeOp::PHYSICS_BODY_WRITE;
    const bool command = request.op == ManagedBridgeOp::PHYSICS_BODY_COMMAND;
    if (!reading && !writing && !command) return ManagedBridgeStatus::UNSUPPORTED;
    if (!target.IsValid()) return ManagedBridgeStatus::NOT_FOUND;
    const bool atPoint = command && (request.index == static_cast<uint32_t>(ManagedPhysicsBodyCommand::FORCE_AT_POINT)
        || request.index == static_cast<uint32_t>(ManagedPhysicsBodyCommand::POINT_IMPULSE));
    const bool freeze = command && (request.index == static_cast<uint32_t>(ManagedPhysicsBodyCommand::FREEZE_POSITION)
        || request.index == static_cast<uint32_t>(ManagedPhysicsBodyCommand::FREEZE_ROTATION));
    if ((freeze ? (request.flags & ~7u) != 0 : request.flags != 0)
        || (atPoint ? request.inputBytes != 12 || !request.input : request.inputBytes != 0))
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    RigidBodyComponent* component = target.GetComponent<RigidBodyComponent>();
    physics::RigidBody* body = component ? component->rigidBody.get() : nullptr;
    if (reading) {
        const auto selector = static_cast<ManagedPhysicsBodyValue>(request.index);
        if (selector == ManagedPhysicsBodyValue::EXISTS) return ReplyFlag(reply, body != nullptr);
        if (selector == ManagedPhysicsBodyValue::ENABLED) return ReplyFlag(reply, body && component->enabled);
        if (!body || !component->enabled) return ManagedBridgeStatus::NOT_FOUND;
        switch (selector) {
        case ManagedPhysicsBodyValue::VELOCITY: return ReplyVector(reply, body->GetVelocity());
        case ManagedPhysicsBodyValue::ANGULAR_VELOCITY: return ReplyVector(reply, body->GetAngularVelocity());
        case ManagedPhysicsBodyValue::POSITION: return ReplyVector(reply, body->GetPosition());
        case ManagedPhysicsBodyValue::MASS: return ReplyScalar(reply, body->GetMass());
        case ManagedPhysicsBodyValue::GRAVITY_SCALE: return ReplyScalar(reply, body->m_gravityScale);
        case ManagedPhysicsBodyValue::IS_STATIC: return ReplyFlag(reply, body->IsStatic());
        case ManagedPhysicsBodyValue::IS_SLEEPING: return ReplyFlag(reply, body->IsSleeping());
        case ManagedPhysicsBodyValue::LINEAR_DRAG: return ReplyScalar(reply, body->m_linearDrag);
        case ManagedPhysicsBodyValue::ANGULAR_DRAG: return ReplyScalar(reply, body->m_angularDrag);
        case ManagedPhysicsBodyValue::CCD_RADIUS: return ReplyScalar(reply, body->m_ccdRadius);
        case ManagedPhysicsBodyValue::LOCAL_TIME_SCALE: return ReplyScalar(reply, body->m_timeScale);
        case ManagedPhysicsBodyValue::FLOW_COUPLING: return ReplyScalar(reply, component->flowCoupling);
        case ManagedPhysicsBodyValue::USE_CCD: return ReplyFlag(reply, body->m_useCCD);
        case ManagedPhysicsBodyValue::USE_GRAVITY: return ReplyFlag(reply, body->m_useGravity);
        case ManagedPhysicsBodyValue::ALLOW_SLEEPING: return ReplyFlag(reply, body->m_allowSleeping);
        default: return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
    }
    if (!fixedUpdate) return ManagedBridgeStatus::INVALID_SCOPE;
    if (!body || !component->enabled) return ManagedBridgeStatus::NOT_FOUND;
    physics::RigidBody candidate = *body;
    const bool refreshInertia = writing && (request.index == static_cast<uint32_t>(ManagedPhysicsBodyValue::MASS)
        || request.index == static_cast<uint32_t>(ManagedPhysicsBodyValue::IS_STATIC));
    const physics::Collider* inertiaSource = refreshInertia ? LiveInertiaSource(target, body->GetInertiaCollider()) : nullptr;
    if (refreshInertia) candidate.SetInertiaFromCollider(inertiaSource);
    if (!(writing ? ApplyWrite(candidate, request) : ApplyCommand(candidate, request)) || !candidate.IsFiniteState())
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (command && request.index == static_cast<uint32_t>(ManagedPhysicsBodyCommand::TELEPORT)) {
        if (!PrepareTeleport(target, candidate.GetPosition())) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const Transform* value = Shadow(target);
        candidate.SetRotation(value->worldRotation);
        if (!candidate.IsFiniteState()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        body->SetPosition(candidate.GetPosition()); body->SetRotation(value->worldRotation);
        CommitTransforms();
        component->ResetPhysicsSyncState(target.transform.worldPosition, body->GetRotation());
        return ManagedBridgeStatus::OK;
    }
    /// @note 候補で検証した同一操作を現在の実体へ一度だけ実行し、World に保持された body pointer を維持する。
    if (refreshInertia) body->SetInertiaFromCollider(inertiaSource);
    if (writing) ApplyWrite(*body, request); else ApplyCommand(*body, request);
    if (writing && request.index == static_cast<uint32_t>(ManagedPhysicsBodyValue::FLOW_COUPLING)) component->flowCoupling = request.x;
    return ManagedBridgeStatus::OK;
}

ManagedBridgeStatus ManagedPhysicsBridge::RunWorld(const physics::World& world,
    const ManagedBridgeRequest& request, ManagedBridgeReply& reply)
{
    if (request.op != ManagedBridgeOp::PHYSICS_WORLD_READ) return ManagedBridgeStatus::UNSUPPORTED;
    if (request.flags || request.inputBytes) return ManagedBridgeStatus::INVALID_ARGUMENT;
    switch (static_cast<ManagedPhysicsWorldValue>(request.index)) {
    case ManagedPhysicsWorldValue::GRAVITY: return ReplyVector(reply, world.GetGravity());
    case ManagedPhysicsWorldValue::LAYERS_COLLIDE:
        if (!std::isfinite(request.x) || !std::isfinite(request.y) || request.x < 0 || request.x > 31
            || request.y < 0 || request.y > 31 || std::floor(request.x) != request.x || std::floor(request.y) != request.y)
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        return ReplyFlag(reply, world.LayersCollide(static_cast<int>(request.x), static_cast<int>(request.y)));
    default: return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
}

ManagedBridgeStatus ManagedPhysicsBridge::RunRotation(GameObject& target, bool writing, std::span<const float> values,
    ManagedServiceResult& result, bool fixedUpdate)
{
    RigidBodyComponent* component = target.GetComponent<RigidBodyComponent>();
    if (!component || !component->rigidBody || !component->enabled) return ManagedBridgeStatus::NOT_FOUND;
    physics::RigidBody& body = *component->rigidBody;
    if (!writing) {
        if (!values.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const math::Quaternion rotation = body.GetRotation();
        if (!Finite(rotation)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        result.values = {rotation.x, rotation.y, rotation.z, rotation.w}; return ManagedBridgeStatus::OK;
    }
    if (!fixedUpdate) return ManagedBridgeStatus::INVALID_SCOPE;
    if (values.size() != 4) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const math::Quaternion rotation{values[0], values[1], values[2], values[3]};
    if (!Normalizable(rotation)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    physics::RigidBody candidate = body;
    candidate.SetRotation(rotation);
    const math::Quaternion candidateRotation = candidate.GetRotation();
    if (!candidate.IsFiniteState() || !PrepareTeleport(target, candidate.GetPosition(), &candidateRotation))
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    const math::Quaternion confirmedRotation = Shadow(target)->worldRotation;
    candidate.SetRotation(confirmedRotation);
    if (!candidate.IsFiniteState()) return ManagedBridgeStatus::INVALID_ARGUMENT;
    body.SetRotation(confirmedRotation);
    CommitTransforms();
    component->ResetPhysicsSyncState(target.transform.worldPosition, body.GetRotation());
    return ManagedBridgeStatus::OK;
}

ManagedBridgeStatus ManagedPhysicsBridge::RunCollider(GameObject& target, uint32_t kind, uint32_t property, bool writing,
    std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result, bool fixedUpdate)
{
    if (!target.IsValid() || !target.GetScene()) return ManagedBridgeStatus::NOT_FOUND;
    ColliderComponent* component = SelectCollider(target, kind);
    if (!component) return ManagedBridgeStatus::NOT_FOUND;
    if (!writing) {
        if (!values.empty() || !text.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        switch (property) {
        case 0: result.integer = component->enabled ? 1 : 0; break;
        case 1: result.integer = component->isTrigger ? 1 : 0; break;
        case 2: result.values = {component->center.x, component->center.y, component->center.z}; break;
        case 3:
            if (kind == 2 || kind == 16) {
                const auto size = kind == 2 ? target.GetComponent<BoxColliderComponent>()->size : target.GetComponent<AabbColliderComponent>()->size;
                result.values = {size.x, size.y, size.z};
            } else if (kind == 3) result.values = {target.GetComponent<SphereColliderComponent>()->radius};
            else if (kind == 4) { const auto& shape = *target.GetComponent<CapsuleColliderComponent>(); result.values = {shape.radius, shape.halfHeight}; }
            else if (kind == 15) { const auto& shape = *target.GetComponent<CylinderColliderComponent>(); result.values = {shape.radius, shape.halfHeight}; }
            else return ManagedBridgeStatus::UNSUPPORTED;
            break;
        case 4: result.values = {component->material.staticFriction}; break;
        case 5: result.values = {component->material.dynamicFriction}; break;
        case 6: result.values = {component->material.restitution}; break;
        case 7: result.values = {component->material.density}; break;
        case 8: result.text = {component->physicsMaterialPath}; break;
        case 9: result.integer = component->attachToParentBody ? 1 : 0; break;
        case 10: {
            math::Vector3 min, max;
            result.integer = ScriptColliderProxy::TryGetPrimitiveWorldBounds(&target, min, max) ? 1 : 0;
            if (result.integer) result.values = {min.x, min.y, min.z, max.x, max.y, max.z};
            break;
        }
        case 11: result.integer = static_cast<int64_t>(component->material.restitutionCombine); break;
        case 12: result.integer = static_cast<int64_t>(component->material.frictionCombine); break;
        case 13: case 14: case 15: {
            if (kind != 17 && kind != 18) return ManagedBridgeStatus::UNSUPPORTED;
            const auto* mesh = kind == 17 ? target.GetComponent<MeshColliderComponent>() : nullptr;
            const auto* hull = kind == 18 ? target.GetComponent<ConvexHullColliderComponent>() : nullptr;
            if (property == 13) result.text = {mesh ? mesh->meshPath : hull->meshPath};
            else if (property == 14) result.integer = mesh ? mesh->meshIndex : hull->meshIndex;
            else result.integer = (mesh ? mesh->useTransformScale : hull->useTransformScale) ? 1 : 0;
            break;
        }
        default: return ManagedBridgeStatus::UNSUPPORTED;
        }
        return ManagedBridgeStatus::OK;
    }
    if (!fixedUpdate) return ManagedBridgeStatus::INVALID_SCOPE;
    for (float value : values) if (!std::isfinite(value)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (property == 8) {
        if (!values.empty() || text.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        component->SetPhysicsMaterialPath(text[0]); return ManagedBridgeStatus::OK;
    }
    if (!text.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (property == 3) {
        if (kind == 2 || kind == 16) {
            if (values.size() != 3 || values[0] <= 0 || values[1] <= 0 || values[2] <= 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
            const math::Vector3 size{values[0], values[1], values[2]};
            if (!ShapeBoundsFinite(target, size * 0.5f) || !std::isfinite(size.x * size.y * size.z)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            ReleaseInertiaSource(target, component->collider.get());
            if (kind == 2) target.GetComponent<BoxColliderComponent>()->SetSize(size); else target.GetComponent<AabbColliderComponent>()->SetSize(size);
        } else if (kind == 3) {
            if (values.size() != 1 || values[0] < 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
            const float radius = values[0];
            if (!ShapeBoundsFinite(target, {radius, radius, radius}) || !std::isfinite(radius * radius * radius)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            ReleaseInertiaSource(target, component->collider.get());
            target.GetComponent<SphereColliderComponent>()->SetRadius(values[0]);
        } else if (kind == 4 || kind == 15) {
            if (values.size() != 2 || values[0] < 0 || values[1] < 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (!ShapeBoundsFinite(target, {values[0], values[0] + values[1], values[0]})
                || !std::isfinite(values[0] * values[0] * (values[0] + values[1]))) return ManagedBridgeStatus::INVALID_ARGUMENT;
            ReleaseInertiaSource(target, component->collider.get());
            if (kind == 4) target.GetComponent<CapsuleColliderComponent>()->SetCapsule(values[0], values[1]);
            else target.GetComponent<CylinderColliderComponent>()->SetCylinder(values[0], values[1]);
        } else return ManagedBridgeStatus::UNSUPPORTED;
        return ManagedBridgeStatus::OK;
    }
    if (property == 2) {
        if (values.size() != 3) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const math::Vector3 center{values[0], values[1], values[2]};
        const math::Vector3& scale = target.transform.worldScale;
        const math::Vector3 scaledCenter{center.x * scale.x, center.y * scale.y, center.z * scale.z};
        if (!Finite(scaledCenter) || !Finite(target.transform.worldPosition
            + target.transform.worldRotation * scaledCenter)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        component->SetCenter({values[0], values[1], values[2]}); return ManagedBridgeStatus::OK;
    }
    if (values.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const float value = values[0];
    if (property == 11 || property == 12) {
        if (value < 0 || value > 4 || std::floor(value) != value) return ManagedBridgeStatus::INVALID_ARGUMENT;
        auto material = component->material;
        if (property == 11) material.restitutionCombine = static_cast<physics::PhysicsMaterialCombine>(static_cast<uint32_t>(value));
        else material.frictionCombine = static_cast<physics::PhysicsMaterialCombine>(static_cast<uint32_t>(value));
        component->SetMaterial(material); return ManagedBridgeStatus::OK;
    }
    if (property == 15) {
        if ((kind != 17 && kind != 18) || (value != 0 && value != 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (kind == 17) target.GetComponent<MeshColliderComponent>()->useTransformScale = value == 1;
        else target.GetComponent<ConvexHullColliderComponent>()->useTransformScale = value == 1;
        return ManagedBridgeStatus::OK;
    }
    if (property == 0 || property == 1 || property == 9) {
        if (value != 0 && value != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (property == 0) component->SetEnabled(value == 1);
        else if (property == 1) component->SetTrigger(value == 1);
        else component->attachToParentBody = value == 1;
        return ManagedBridgeStatus::OK;
    }
    if (property < 4 || property > 7 || value < 0 || (property == 6 && value > 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    auto material = component->material;
    if (property == 4) material.staticFriction = value;
    if (property == 5) material.dynamicFriction = value;
    if (property == 6) material.restitution = value;
    if (property == 7) material.density = value;
    component->SetMaterial(material); return ManagedBridgeStatus::OK;
}

ManagedBridgeStatus ManagedPhysicsBridge::RunColliderCommand(GameObject& target, uint32_t kind, uint32_t command,
    std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result, bool fixedUpdate)
{
    if (!fixedUpdate) return ManagedBridgeStatus::INVALID_SCOPE;
    ColliderComponent* component = SelectCollider(target, kind);
    if (!component) return ManagedBridgeStatus::NOT_FOUND;
    if (command == 0) {
        if (!values.empty() || text.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const auto* material = physics::PhysicsMaterial::FindPreset(text[0].c_str());
        result.integer = material ? 1 : 0;
        if (material) component->SetMaterial(*material);
        return ManagedBridgeStatus::OK;
    }
    if (command == 1) {
        if ((kind != 17 && kind != 18) || !values.empty() || text.size() != 2) return ManagedBridgeStatus::INVALID_ARGUMENT;
        int index = 0;
        const auto parsed = std::from_chars(text[1].data(), text[1].data() + text[1].size(), index);
        if (parsed.ec != std::errc{} || parsed.ptr != text[1].data() + text[1].size() || index < 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        ReleaseInertiaSource(target, component->collider.get());
        if (kind == 17) target.GetComponent<MeshColliderComponent>()->SetMesh(text[0], index);
        else target.GetComponent<ConvexHullColliderComponent>()->SetMesh(text[0], index);
        return ManagedBridgeStatus::OK;
    }
    return ManagedBridgeStatus::UNSUPPORTED;
}

ManagedBridgeStatus ManagedPhysicsBridge::MoveWorld(GameObject& target, const math::Vector3& position, bool fixedUpdate)
{
    if (!Finite(position)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (auto* component = target.GetComponent<RigidBodyComponent>(); component && component->enabled && component->rigidBody) {
        ManagedBridgeRequest request{}; ManagedBridgeReply reply{};
        request.op = ManagedBridgeOp::PHYSICS_BODY_COMMAND; request.index = static_cast<uint32_t>(ManagedPhysicsBodyCommand::TELEPORT);
        request.x = position.x; request.y = position.y; request.z = position.z;
        return RunBody(target, request, reply, fixedUpdate);
    }
    if (!PrepareTeleport(target, position)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    CommitTransforms(); return ManagedBridgeStatus::OK;
}

ManagedBridgeStatus ManagedPhysicsBridge::Query(Scene& scene, const physics::World& world,
    const ManagedBridgeRequest& request, std::vector<ManagedPhysicsHit>& hits, GameObject* ignored)
{
    const bool filtered = request.op >= ManagedBridgeOp::PHYSICS_RAYCAST_FILTERED && request.op <= ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE_FILTERED;
    const bool ray = request.op == ManagedBridgeOp::PHYSICS_RAYCAST || request.op == ManagedBridgeOp::PHYSICS_RAYCAST_FILTERED;
    const bool all = request.op == ManagedBridgeOp::PHYSICS_RAYCAST_ALL || request.op == ManagedBridgeOp::PHYSICS_RAYCAST_ALL_FILTERED;
    const bool sphere = request.op == ManagedBridgeOp::PHYSICS_SPHERE_CAST || request.op == ManagedBridgeOp::PHYSICS_SPHERE_CAST_FILTERED;
    const bool overlap = request.op == ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE || request.op == ManagedBridgeOp::PHYSICS_OVERLAP_SPHERE_FILTERED;
    if (!ray && !all && !sphere && !overlap) return ManagedBridgeStatus::UNSUPPORTED;
    const uint32_t baseBytes = overlap ? 0u : sphere ? 16u : 12u;
    const uint32_t inputBytes = baseBytes + (filtered ? 16u : 0u);
    if (request.flags || request.index || request.inputBytes != inputBytes || (inputBytes && !request.input)
        || !Finite(Vector(request)) || !std::isfinite(request.w) || request.w < 0)
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    const math::Vector3 direction = overlap ? math::Vector3::ZERO : InputVector(request);
    float radius = overlap ? request.w : 0.0f;
    if (sphere) std::memcpy(&radius, request.input + 12, sizeof(radius));
    if (!Finite(direction) || !std::isfinite(direction.LengthSq()) || !std::isfinite(radius) || radius < 0)
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    uint32_t mask = UINT32_MAX;
    uint32_t triggers = 0;
    if (filtered) {
        std::memcpy(&mask, request.input + baseBytes, 4);
        std::memcpy(&triggers, request.input + baseBytes + 4, 4);
        if (triggers > 2) return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    /// @note Owner results exist only during this readonly query. No raw owner pointer survives deletion, Play/Stop or Scene replacement.
    /// @note Stable names and category match the baseline capture; valid query scope counts are actual call counts.
    const char* queryName = overlap ? "ManagedPhysicsBridge::Query.OverlapSphere"
        : all ? "ManagedPhysicsBridge::Query.RaycastAll" : sphere ? "ManagedPhysicsBridge::Query.SphereCast"
        : "ManagedPhysicsBridge::Query.Raycast";
    const profiler::ProfileScope queryScope(profiler::ProfilerMarker{queryName,"PhysicsQuery"});
    ColliderOwners owners;
    bool completeOwners = all || overlap;
    if (completeOwners) CollectQueryColliderOwners(scene,owners);
    constexpr size_t DIRECT_OWNER_LIMIT = 8;
    size_t directOwners = 0;
    const auto resolveOwner = [&](const physics::Collider* source) -> GameObject* {
    const profiler::ProfileScope ownerResolveScope(profiler::ProfilerMarker{"ManagedPhysicsBridge::Query.OwnerResolve", "PhysicsQuery"});
        if (!source) return nullptr;
        if (const auto found = owners.find(source); found != owners.end()) return found->second;
        if (!completeOwners && ++directOwners >= DIRECT_OWNER_LIMIT) {
            CollectQueryColliderOwners(scene,owners); completeOwners = true;
        }
        if (completeOwners) {
            const auto found = owners.find(source);
            return found == owners.end() ? nullptr : found->second;
        }
        GameObject* owner = FindQueryColliderOwner(scene,source);
        owners.emplace(source,owner);
        return owner;
    };
    /// @note World の非所有ポインターは削除・Scene 切替後も残り得る。現 Scene の所有 pointer と一致するまで参照先を読まない。
    const auto filter = [&](const physics::ColliderInstance& instance) {
        GameObject* owner = resolveOwner(instance.collider);
        return owner && owner != ignored && owner->layer >= 0 && owner->layer < 32
            && ((mask >> static_cast<uint32_t>(owner->layer)) & 1u) != 0
            && (triggers == 0 || (triggers == 1 ? !instance.isTrigger : instance.isTrigger));
    };
    std::vector<ManagedPhysicsHit> result;
    const auto append = [&](const physics::World::RaycastHit& hit) {
        GameObject* owner = resolveOwner(hit.collider);
        if (!owner || !Finite(hit.point) || !Finite(hit.normal) || !std::isfinite(hit.distance) || hit.distance < 0)
            return false;
        result.push_back({owner, hit.point, hit.normal, hit.distance});
        return true;
    };
    if (overlap) {
        std::vector<const physics::ColliderInstance*> worldHits;
        {
    const profiler::ProfileScope worldScope(profiler::ProfilerMarker{"ManagedPhysicsBridge::Query.World", "PhysicsQuery"});
            worldHits = world.OverlapSphere(Vector(request),radius,filter);
        }
        std::unordered_set<GameObject*> seen;
        for (const physics::ColliderInstance* hit : worldHits) {
            GameObject* owner = resolveOwner(hit->collider);
            if (owner && seen.insert(owner).second) result.push_back({owner});
        }
    } else if (all) {
        std::vector<physics::World::RaycastHit> worldHits;
        {
    const profiler::ProfileScope worldScope(profiler::ProfilerMarker{"ManagedPhysicsBridge::Query.World", "PhysicsQuery"});
            worldHits = world.RaycastAll(Vector(request),direction,request.w,filter);
        }
        for (const physics::World::RaycastHit& hit : worldHits)
            if (!append(hit)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    } else {
        physics::World::RaycastHit hit;
        bool found = false;
        {
    const profiler::ProfileScope worldScope(profiler::ProfilerMarker{"ManagedPhysicsBridge::Query.World", "PhysicsQuery"});
            found = sphere ? world.SphereCast(Vector(request),radius,direction,request.w,hit,filter)
                : world.Raycast(Vector(request),direction,request.w,hit,filter);
        }
        if (found && !append(hit)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    hits.swap(result);
    return ManagedBridgeStatus::OK;
}

} /// @note namespace fbzz::scene::managed_detail
