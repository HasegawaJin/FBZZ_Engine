/// @file    ManagedTransformBatch.cpp
/// @brief   作業用 Transform で事前計算してから一括操作を確定する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include "ManagedTransformBatch.hpp"
#include "Engine/Scene/Components/RigidBodyComponent.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Systems/TransformSystem.hpp"
#include "Math/MathUtils.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace fbzz::scene::managed_detail {
namespace {

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Finite(const math::Quaternion& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z) && std::isfinite(value.w);
}

bool Finite(const Transform& transform)
{
    return Finite(transform.position) && Finite(transform.rotation) && Finite(transform.scale)
        && Finite(transform.worldPosition) && Finite(transform.worldRotation) && Finite(transform.worldScale);
}

/// @note 正規化と逆回転は別のゼロ判定を持つため、各 MathContract の事前条件も確認する。
/// @see Projects/Math/src/Quaternion.cpp Quaternion::Normalized / Quaternion::Inverse。
bool FiniteNormalization(const math::Quaternion& value)
{
    if (!Finite(value)) return false;
    const float length = value.Length();
    return std::isfinite(length) && !math::NearlyZero(length);
}

bool FiniteInverse(const math::Quaternion& value)
{
    if (!Finite(value)) return false;
    const float lengthSq = value.x * value.x + value.y * value.y
        + value.z * value.z + value.w * value.w;
    return std::isfinite(lengthSq) && !math::NearlyZero(lengthSq);
}

template<typename T>
T Read(const char* bytes)
{
    T value{};
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

template<typename T>
void Write(char* bytes, const T& value)
{
    std::memcpy(bytes, &value, sizeof(value));
}

} /// @note namespace

Transform* ManagedTransformBatch::Shadow(GameObject& object)
{
    const uint32_t index = object.GetID().index;
    if (index >= m_shadow.size()) return nullptr;
    if (m_cloneEpoch[index] != m_epoch) {
        m_shadow[index] = object.transform;
        m_cloneEpoch[index] = m_epoch;
    }
    return &m_shadow[index];
}

bool ManagedTransformBatch::Update(GameObject& object, bool shadow)
{
    GameObject* parent = object.GetParent();
    Transform* transform = shadow ? Shadow(object) : &object.transform;
    const Transform* parentTransform = parent ? (shadow ? Shadow(*parent) : &parent->transform) : nullptr;
    if (!transform || (parent && !parentTransform)) return false;
    if (shadow) {
        if (!Finite(transform->position) || !Finite(transform->scale)
            || !FiniteNormalization(transform->rotation)) return false;
        if (parentTransform && (!Finite(parentTransform->worldPosition)
            || !Finite(parentTransform->worldScale) || !FiniteNormalization(parentTransform->worldRotation)
            || !FiniteNormalization(parentTransform->worldRotation * transform->rotation))) return false;
    }
    UpdateWorldTransform(*transform, parentTransform);
    return !shadow || Finite(*transform);
}

bool ManagedTransformBatch::Synchronize(GameObject& root, bool shadow)
{
    Scene* scene = root.GetScene();
    if (!scene) return false;
    const size_t slots = scene->EntitySlotCount();
    m_ancestors.clear();
    for (GameObject* parent = root.GetParent(); parent; parent = parent->GetParent()) {
        if (m_ancestors.size() >= slots || parent->GetScene() != scene
            || scene->GetGameObject(parent->GetID()) != parent) return false;
        m_ancestors.push_back(parent);
    }
    for (auto it = m_ancestors.rbegin(); it != m_ancestors.rend(); ++it)
        if (!Update(**it, shadow)) return false;
    m_queue.clear();
    m_queue.push_back(&root);
    for (size_t i = 0; i < m_queue.size(); ++i) {
        GameObject& object = *m_queue[i];
        if (object.GetScene() != scene || scene->GetGameObject(object.GetID()) != &object
            || !Update(object, shadow)) return false;
        for (int childIndex = 0; childIndex < object.GetChildCount(); ++childIndex) {
            if (GameObject* child = object.GetChild(childIndex)) {
                if (m_queue.size() >= slots || child->GetParent() != &object) return false;
                m_queue.push_back(child);
            }
        }
    }
    return true;
}

bool ManagedTransformBatch::PrepareWrite(Entry& entry, bool translate, const math::Vector3& displacement)
{
    GameObject& object = *entry.object;
    Transform* transform = Shadow(object);
    if (!transform) return false;
    /// @note 最初の world 読み取りは描画補間も含む現在値。setter の祖先同期より前に加算する。
    if (translate) entry.position = transform->worldPosition + displacement;
    if (!Finite(entry.position) || !Synchronize(object, true)) return false;
    const math::Quaternion localRotation = transform->rotation;
    const math::Vector3 localScale = transform->scale;
    GameObject* parent = object.GetParent();
    const Transform* parentTransform = parent ? Shadow(*parent) : nullptr;
    if (parent && !parentTransform) return false;
    if (parentTransform) {
        if (!FiniteInverse(parentTransform->worldRotation)) return false;
        const math::Quaternion inverse = parentTransform->worldRotation.Inverse();
        if (!Finite(inverse) || !FiniteNormalization(inverse * transform->worldRotation)) return false;
        /// @note ゼロスケール投影で隠れる非有限値も、親との差分と逆回転の段階で拒否する。
        const math::Vector3 offset = entry.position - parentTransform->worldPosition;
        if (!Finite(offset) || !Finite(inverse * offset)) return false;
    }
    SetWorldPose(*transform, parentTransform, entry.position, transform->worldRotation, transform->worldScale);
    /// @note 位置操作は local 回転とスケールを変更しない。逆算の非有限値は復元で隠す前に拒否する。
    if (!Finite(*transform)) return false;
    transform->rotation = localRotation;
    transform->scale = localScale;
    return Synchronize(object, true);
}

void ManagedTransformBatch::ApplyWrite(const Entry& entry)
{
    GameObject& object = *entry.object;
    Synchronize(object, false);
    const math::Quaternion localRotation = object.transform.rotation;
    const math::Vector3 localScale = object.transform.scale;
    SetWorldPose(object, entry.position, object.transform.worldRotation, object.transform.worldScale);
    object.transform.rotation = localRotation;
    object.transform.scale = localScale;
    Synchronize(object, false);
}

bool ManagedTransformBatch::CanStageIndependentRoots(const Scene& scene)
{
    BeginEpoch(scene);
    for (const Entry& entry : m_entries) {
        if (!entry.apply) continue;
        GameObject& object = *entry.object;
        const uint32_t index = object.GetID().index;
        if (object.GetScene() != &scene || scene.GetGameObject(object.GetID()) != &object
            || object.GetParent() || object.GetChildCount() != 0
            || index >= m_cloneEpoch.size() || m_cloneEpoch[index] == m_epoch) return false;
        m_cloneEpoch[index] = m_epoch;
    }
    return true;
}

bool ManagedTransformBatch::StageIndependentRoots(bool translate, const math::Vector3& displacement)
{
    m_rootShadow.resize(m_entries.size());
    for (size_t index = 0; index < m_entries.size(); ++index)
        if (m_entries[index].apply) m_rootShadow[index] = m_entries[index].object->transform;
    for (size_t index = 0; index < m_entries.size(); ++index) {
        Entry& entry = m_entries[index];
        if (!entry.apply) continue;
        Transform& transform = m_rootShadow[index];
        /// @note translate は local 同期前の world キャッシュに加算し、既存 setter と同じ初期値を使う。
        if (translate) entry.position = transform.worldPosition + displacement;
        if (!Finite(entry.position) || !Finite(transform.position) || !Finite(transform.scale)
            || !FiniteNormalization(transform.rotation)) return false;
        /// @note 独立ルートの位置変更後は local 回転・拡縮を保持し、world はそれぞれの local と一致する。
        transform.position = entry.position;
        UpdateWorldTransform(transform, nullptr);
        if (!Finite(transform)) return false;
    }
    return true;
}

void ManagedTransformBatch::CommitIndependentRoots()
{
    for (size_t index = 0; index < m_entries.size(); ++index)
        if (m_entries[index].apply) m_entries[index].object->transform = m_rootShadow[index];
}

void ManagedTransformBatch::BeginEpoch(const Scene& scene)
{
    const size_t slots = scene.EntitySlotCount();
    if (m_cloneEpoch.size() != slots) m_cloneEpoch.resize(slots);
    if (++m_epoch == 0) {
        std::fill(m_cloneEpoch.begin(), m_cloneEpoch.end(), 0);
        m_epoch = 1;
    }
}

void ManagedTransformBatch::BeginShadow(const Scene& scene)
{
    BeginEpoch(scene);
    const size_t slots = scene.EntitySlotCount();
    if (m_shadow.size() != slots) m_shadow.resize(slots);
}

void ManagedTransformBatch::CommitShadow()
{
    for (GameObject* object : m_ancestors) object->transform = *Shadow(*object);
    for (GameObject* object : m_queue) object->transform = *Shadow(*object);
}

ManagedBridgeStatus ManagedTransformBatch::RunPosition(GameObject& target,
    const ManagedBridgeRequest& request)
{
    if (request.op != ManagedBridgeOp::TRANSFORM_WRITE_LOCAL
        && request.op != ManagedBridgeOp::TRANSFORM_WRITE_WORLD
        && request.op != ManagedBridgeOp::TRANSFORM_TRANSLATE_LOCAL)
        return ManagedBridgeStatus::UNSUPPORTED;
    if (request.flags || request.index || request.inputBytes) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const math::Vector3 value{request.x, request.y, request.z};
    if (!Finite(value)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (!target.GetScene()) return ManagedBridgeStatus::NOT_FOUND;
    BeginShadow(*target.GetScene());
    if (request.op == ManagedBridgeOp::TRANSFORM_WRITE_WORLD) {
        Entry entry;
        entry.object = &target;
        entry.position = value;
        if (!PrepareWrite(entry, false, math::Vector3::ZERO)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    } else {
        Transform* transform = Shadow(target);
        if (!transform) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (request.op == ManagedBridgeOp::TRANSFORM_WRITE_LOCAL) transform->position = value;
        else transform->Translate(value);
        if (!Synchronize(target, true)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    CommitShadow();
    return ManagedBridgeStatus::OK;
}

ManagedBridgeStatus ManagedTransformBatch::RunPose(GameObject& target,
    const ManagedBridgeRequest& request, ManagedBridgeReply& reply)
{
    if (request.flags || request.inputBytes) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const auto vectorReply = [&](const math::Vector3& value) {
        reply.valueKind = ManagedValueKind::VECTOR3;
        reply.x = value.x;
        reply.y = value.y;
        reply.z = value.z;
        return ManagedBridgeStatus::OK;
    };
    switch (request.op) {
    case ManagedBridgeOp::TRANSFORM_READ_ROTATION: {
        if (request.index > 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const math::Quaternion& rotation = request.index ? target.transform.worldRotation : target.transform.rotation;
        reply.x = rotation.x;
        reply.y = rotation.y;
        reply.z = rotation.z;
        reply.w = rotation.w;
        return ManagedBridgeStatus::OK;
    }
    case ManagedBridgeOp::TRANSFORM_READ_SCALE:
        if (request.index) return ManagedBridgeStatus::INVALID_ARGUMENT;
        return vectorReply(target.transform.scale);
    case ManagedBridgeOp::TRANSFORM_READ_DIRECTION:
        if (request.index > 2) return ManagedBridgeStatus::INVALID_ARGUMENT;
        return vectorReply(request.index == 0 ? target.transform.Forward()
            : request.index == 1 ? target.transform.Right() : target.transform.Up());
    case ManagedBridgeOp::TRANSFORM_WRITE_LOCAL_ROTATION:
    case ManagedBridgeOp::TRANSFORM_WRITE_LOCAL_SCALE:
    case ManagedBridgeOp::TRANSFORM_ROTATE:
    case ManagedBridgeOp::TRANSFORM_LOOK_AT:
        break;
    default: return ManagedBridgeStatus::UNSUPPORTED;
    }
    if (request.index) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const auto* body = target.GetComponent<RigidBodyComponent>();
    if (body && body->enabled && body->rigidBody) return ManagedBridgeStatus::PHYSICS_BODY;
    const math::Vector3 value{request.x, request.y, request.z};
    if (!Finite(value)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (request.op == ManagedBridgeOp::TRANSFORM_ROTATE) {
        const float lengthSq = value.LengthSq();
        if (!std::isfinite(request.w) || !std::isfinite(lengthSq)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        /// @note 零軸は C++ proxy と同じ完全な no-op。描画補間された world キャッシュも同期しない。
        if (lengthSq < math::EPSILON * math::EPSILON) return ManagedBridgeStatus::OK;
    }
    if (!target.GetScene()) return ManagedBridgeStatus::NOT_FOUND;
    BeginShadow(*target.GetScene());
    Transform* transform = Shadow(target);
    if (!transform) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (request.op == ManagedBridgeOp::TRANSFORM_WRITE_LOCAL_ROTATION) {
        const math::Quaternion rotation{request.x, request.y, request.z, request.w};
        if (!FiniteNormalization(rotation)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        transform->rotation = rotation;
    } else if (request.op == ManagedBridgeOp::TRANSFORM_WRITE_LOCAL_SCALE) {
        transform->scale = value;
    } else {
        if (!Synchronize(target, true)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (request.op == ManagedBridgeOp::TRANSFORM_ROTATE) {
            /// @note C++ ScriptTransformProxy と同じ親のローカル軸・度単位・左積の回転。
            /// @see Projects/Engine/src/Scene/ScriptProxies.cpp ScriptTransformProxy::Rotate。
            const math::Quaternion delta = math::Quaternion::FromAxisAngle(value.Normalized(), request.w * math::DEG2RAD);
            const math::Quaternion rotation = delta * transform->rotation;
            if (!FiniteNormalization(rotation)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            transform->rotation = rotation.Normalized();
        } else {
            const math::Vector3 direction = value - transform->worldPosition;
            const float lengthSq = direction.LengthSq();
            if (!Finite(direction) || !std::isfinite(lengthSq)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (lengthSq >= 1e-6f) {
                if (!FiniteInverse(transform->rotation)) return ManagedBridgeStatus::INVALID_ARGUMENT;
                const math::Quaternion parentRotation = transform->worldRotation * transform->rotation.Inverse();
                if (!FiniteInverse(parentRotation)) return ManagedBridgeStatus::INVALID_ARGUMENT;
                /// @note 自分と重なる対象は回転しない。親子計算を変更せず、MathContract の前提を満たしてから呼ぶ。
                /// @see Projects/Engine/src/Scene/Transform.cpp Transform::LookAt。
                transform->LookAt(value);
            }
        }
    }
    if (!Synchronize(target, true)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    CommitShadow();
    return ManagedBridgeStatus::OK;
}

ManagedBridgeStatus ManagedTransformBatch::Run(Scene& scene, const ManagedBridgeRequest& request,
    ManagedBridgeReply& reply, ObjectResolver resolver, void* resolverState)
{
    if ((request.inputBytes && !request.input) || (request.outputCapacity && !request.output)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    return RunAggregate(scene, request, {request.input, request.inputBytes}, {request.output, request.outputCapacity}, reply, resolver, resolverState);
}

ManagedBridgeStatus ManagedTransformBatch::RunAggregate(Scene& scene, const ManagedBridgeRequest& request,
    std::span<const char> input, std::span<char> output, ManagedBridgeReply& reply,
    ObjectResolver resolver, void* resolverState, bool publish)
{
    reply.index = 0;
    reply.flags = 0;
    reply.outputBytes = 0;
    const bool read = request.op == ManagedBridgeOp::OBJECTS_READ_WORLD_POSITIONS;
    const bool set = request.op == ManagedBridgeOp::OBJECTS_SET_WORLD_POSITIONS;
    const bool translate = request.op == ManagedBridgeOp::OBJECTS_TRANSLATE_WORLD;
    if ((!read && !set && !translate) || !resolver
        || (translate ? (request.flags & ~1u) != 0 : request.flags != 0))
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    const uint32_t inputStride = set ? kManagedBatchSetItemBytes : kManagedBatchReferenceBytes;
    const uint32_t outputStride = read ? kManagedBatchReadItemBytes : (set ? kManagedBatchStatusBytes : 0u);
    if (input.size() != static_cast<uint64_t>(request.index) * inputStride)
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    const math::Vector3 displacement{request.x, request.y, request.z};
    if (translate && !Finite(displacement)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const uint64_t outputBytes = static_cast<uint64_t>(request.index) * outputStride;
    reply.outputBytes = outputBytes <= UINT32_MAX ? static_cast<uint32_t>(outputBytes) : 0;
    if (outputBytes > output.size())
        return ManagedBridgeStatus::BUFFER_TOO_SMALL;

    m_entries.resize(request.index);
    const uint64_t lifetime = scene.GetRenderSceneGeneration();
    uint32_t applied = 0;
    for (uint32_t i = 0; i < request.index; ++i) {
        Entry& entry = m_entries[i];
        entry = Entry{};
        const char* row = input.data() + static_cast<size_t>(i) * inputStride;
        const uint64_t objectId = Read<uint64_t>(row);
        const uint64_t rowLifetime = Read<uint64_t>(row + sizeof(uint64_t));
        if (!objectId && !rowLifetime) continue;
        if (rowLifetime != lifetime) return ManagedBridgeStatus::SCENE_MISMATCH;
        entry.status = ManagedObjectStatus::EXPIRED;
        if (!objectId) continue;
        const auto status = resolver(resolverState, objectId, entry.object);
        if (status != ManagedBridgeStatus::OK) return status;
        if (!entry.object) continue;
        if (translate && (request.flags & 1u) && !entry.object->activeInHierarchy()) continue;
        if (!read) {
            const auto* body = entry.object->GetComponent<RigidBodyComponent>();
            if (body && body->enabled && body->rigidBody) return ManagedBridgeStatus::PHYSICS_BODY;
        }
        entry.status = ManagedObjectStatus::VALID;
        entry.apply = true;
        ++applied;
        if (read) {
            entry.position = entry.object->transform.worldPosition;
            if (!Finite(entry.position)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        } else if (set) {
            entry.position = {Read<float>(row + 16), Read<float>(row + 20), Read<float>(row + 24)};
            if (!Finite(entry.position)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
    }

    if (!read && applied) {
        if (CanStageIndependentRoots(scene)) {
            if (!StageIndependentRoots(translate, displacement)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (publish) CommitIndependentRoots();
        } else {
            BeginShadow(scene);
            for (Entry& entry : m_entries)
                if (entry.apply && !PrepareWrite(entry, translate, displacement))
                    return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (publish) for (const Entry& entry : m_entries)
                if (entry.apply) ApplyWrite(entry);
        }
    }

    /// @note output の借用領域へは成功が確定してから書く。失効 slot の位置はゼロのまま残す。
    for (uint32_t i = 0; i < request.index && outputStride; ++i) {
        const Entry& entry = m_entries[i];
        char* row = output.data() + static_cast<size_t>(i) * outputStride;
        Write(row, static_cast<uint32_t>(entry.status));
        if (read) {
            Write(row + 4, entry.position.x);
            Write(row + 8, entry.position.y);
            Write(row + 12, entry.position.z);
        }
    }
    reply.index = applied;
    reply.flags = request.index - applied;
    return ManagedBridgeStatus::OK;
}

} /// @note namespace fbzz::scene::managed_detail
