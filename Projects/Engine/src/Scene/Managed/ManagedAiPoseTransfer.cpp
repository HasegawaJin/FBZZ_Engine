/// @file    ManagedAiPoseTransfer.cpp
/// @brief   Validate borrowed AI pose packets and stage hierarchy math before publication.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include "ManagedAiPoseTransfer.hpp"
#include "Engine/Scene/Components/RigidBodyComponent.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Systems/TransformSystem.hpp"
#include "Math/MathUtils.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace fbzz::scene::managed_detail {
namespace {
constexpr uint32_t HEADER_BYTES = 4, ROW_BYTES = 44, STATUS_BYTES = 4, MAX_AGENTS = 4096;

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool Finite(const math::Quaternion& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z) && std::isfinite(value.w);
}
bool Finite(const Transform& value)
{
    return Finite(value.position) && Finite(value.rotation) && Finite(value.scale)
        && Finite(value.worldPosition) && Finite(value.worldRotation) && Finite(value.worldScale);
}
/// @note Match Quaternion's normalization and inverse preconditions before invoking Math contracts.
/// @see Projects/Math/src/Quaternion.cpp Quaternion::Normalized / Quaternion::Inverse.
bool Normalizable(const math::Quaternion& value)
{
    const float length = value.Length();
    return Finite(value) && std::isfinite(length) && !math::NearlyZero(length);
}
bool Invertible(const math::Quaternion& value)
{
    const float squared = value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
    return Finite(value) && std::isfinite(squared) && !math::NearlyZero(squared);
}
template<typename T> T Read(const char* bytes)
{
    T result{};
    std::memcpy(&result, bytes, sizeof(result));
    return result;
}
} /// @note namespace

void ManagedAiPoseTransfer::BeginEpoch(const Scene& scene)
{
    m_publish.clear();
    const size_t slots = scene.EntitySlotCount();
    m_shadow.resize(slots);
    m_cloneEpoch.resize(slots);
    m_targetEpoch.resize(slots);
    if (++m_epoch == 0) {
        std::fill(m_cloneEpoch.begin(), m_cloneEpoch.end(), 0);
        std::fill(m_targetEpoch.begin(), m_targetEpoch.end(), 0);
        m_epoch = 1;
    }
}

Transform* ManagedAiPoseTransfer::Shadow(GameObject& object)
{
    const uint32_t index = object.GetID().index;
    if (index >= m_shadow.size()) return nullptr;
    if (m_cloneEpoch[index] != m_epoch) {
        m_shadow[index] = object.transform;
        m_cloneEpoch[index] = m_epoch;
        m_publish.push_back(&object);
    }
    return &m_shadow[index];
}

bool ManagedAiPoseTransfer::Update(GameObject& object)
{
    Transform* transform = Shadow(object);
    GameObject* parent = object.GetParent();
    const Transform* parentTransform = parent ? Shadow(*parent) : nullptr;
    if (!transform || (parent && !parentTransform) || !Finite(transform->position)
        || !Finite(transform->scale) || !Normalizable(transform->rotation)) return false;
    if (parentTransform && (!Finite(parentTransform->worldPosition) || !Finite(parentTransform->worldScale)
        || !Normalizable(parentTransform->worldRotation)
        || !Normalizable(parentTransform->worldRotation * transform->rotation))) return false;
    UpdateWorldTransform(*transform, parentTransform);
    return Finite(*transform);
}

bool ManagedAiPoseTransfer::Synchronize(GameObject& root)
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
        if (!Update(**it)) return false;
    m_queue.clear();
    m_queue.push_back(&root);
    for (size_t index = 0; index < m_queue.size(); ++index) {
        GameObject& object = *m_queue[index];
        if (object.GetScene() != scene || scene->GetGameObject(object.GetID()) != &object || !Update(object)) return false;
        for (int childIndex = 0; childIndex < object.GetChildCount(); ++childIndex) {
            if (GameObject* child = object.GetChild(childIndex)) {
                if (m_queue.size() >= slots || child->GetParent() != &object) return false;
                m_queue.push_back(child);
            }
        }
    }
    return true;
}

bool ManagedAiPoseTransfer::ValidateHierarchy(const Scene& scene)
{
    for (const Entry& entry : m_entries) {
        if (!entry.object) continue;
        size_t depth = 0;
        for (GameObject* parent = entry.object->GetParent(); parent; parent = parent->GetParent()) {
            const uint32_t index = parent->GetID().index;
            if (++depth > scene.EntitySlotCount() || parent->GetScene() != &scene
                || scene.GetGameObject(parent->GetID()) != parent || index >= m_targetEpoch.size()
                || m_targetEpoch[index] == m_epoch) return false;
        }
    }
    return true;
}

bool ManagedAiPoseTransfer::Stage(Entry& entry)
{
    GameObject& object = *entry.object;
    if (!Synchronize(object)) return false;
    Transform* transform = Shadow(object);
    GameObject* parent = object.GetParent();
    const Transform* parentTransform = parent ? Shadow(*parent) : nullptr;
    if (!transform || (parent && !parentTransform)) return false;
    if (parentTransform) {
        if (!Invertible(parentTransform->worldRotation)) return false;
        const math::Quaternion inverse = parentTransform->worldRotation.Inverse();
        const math::Vector3 offset = entry.position - parentTransform->worldPosition;
        if (!Finite(inverse) || !Finite(offset) || !Finite(inverse * offset)
            || !Normalizable(inverse * entry.rotation)) return false;
    }
    const math::Vector3 localScale = transform->scale;
    SetWorldPose(*transform, parentTransform, entry.position, entry.rotation, transform->worldScale);
    if (!Finite(*transform)) return false;
    /// @note Position/rotation commits preserve authored local scale, including a parent's zero-scale axes.
    transform->scale = localScale;
    return Synchronize(object);
}

ManagedBridgeStatus ManagedAiPoseTransfer::Run(Scene& scene, const ManagedBridgeRequest& request,
    ManagedBridgeReply& reply, ObjectResolver resolver, void* resolverState)
{
    reply.index = reply.flags = reply.outputBytes = 0;
    if (request.op != ManagedBridgeOp::AI_CROWD_STEP || request.index != 1 || request.flags || !resolver
        || request.reserved0 || request.reserved1 || request.x != 0 || request.y != 0 || request.z != 0 || request.w != 0
        || request.integer < 0 || request.integer > MAX_AGENTS || !request.input
        || (request.outputCapacity && !request.output)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const uint32_t count = static_cast<uint32_t>(request.integer);
    if (request.inputBytes != HEADER_BYTES + count * ROW_BYTES
        || Read<uint32_t>(request.input) != count) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const uint32_t outputBytes = count * STATUS_BYTES;
    reply.outputBytes = outputBytes;
    if (request.outputCapacity < outputBytes) return ManagedBridgeStatus::BUFFER_TOO_SMALL;
    /// @note Overlap cannot corrupt the caller's input snapshot while publishing its status packet.
    const std::less<const char*> before;
    if (outputBytes && before(request.input, request.output + outputBytes)
        && before(request.output, request.input + request.inputBytes)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    BeginEpoch(scene);
    m_entries.resize(count);
    const uint64_t lifetime = scene.GetRenderSceneGeneration();
    uint32_t applied = 0;
    for (uint32_t index = 0; index < count; ++index) {
        Entry& entry = m_entries[index];
        entry = Entry{};
        const char* row = request.input + HEADER_BYTES + index * ROW_BYTES;
        const uint64_t id = Read<uint64_t>(row), rowLifetime = Read<uint64_t>(row + 8);
        if (!id && !rowLifetime) continue;
        if (rowLifetime != lifetime) return ManagedBridgeStatus::SCENE_MISMATCH;
        entry.status = ManagedObjectStatus::EXPIRED;
        if (!id) continue;
        const ManagedBridgeStatus status = resolver(resolverState, id, entry.object);
        if (status != ManagedBridgeStatus::OK) return status;
        if (!entry.object) continue;
        const uint32_t slot = entry.object->GetID().index;
        if (entry.object->GetScene() != &scene) return ManagedBridgeStatus::SCENE_MISMATCH;
        if (scene.GetGameObject(entry.object->GetID()) != entry.object || slot >= m_shadow.size())
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        const auto* body = entry.object->GetComponent<RigidBodyComponent>();
        if (body && body->enabled && body->rigidBody) return ManagedBridgeStatus::PHYSICS_BODY;
        entry.position = {Read<float>(row + 16), Read<float>(row + 20), Read<float>(row + 24)};
        entry.rotation = {Read<float>(row + 28), Read<float>(row + 32), Read<float>(row + 36), Read<float>(row + 40)};
        if (!Finite(entry.position) || !Normalizable(entry.rotation)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        entry.rotation = entry.rotation.Normalized();
        if (!Normalizable(entry.rotation)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        m_targetEpoch[slot] = m_epoch;
        entry.status = ManagedObjectStatus::VALID;
        ++applied;
    }
    if (!ValidateHierarchy(scene)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    for (Entry& entry : m_entries)
        if (entry.object && !Stage(entry)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    /// @note No callbacks or resolver calls occur after validation; all staged hierarchy calculations have succeeded.
    /// @note The final shadow already contains every validated ancestor and descendant world pose.
    /// @note Publish those values once; a third hierarchy calculation would repeat identical arithmetic.
    for (GameObject* object : m_publish)
        object->transform = m_shadow[object->GetID().index];
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t status = static_cast<uint32_t>(m_entries[index].status);
        std::memcpy(request.output + index * STATUS_BYTES, &status, sizeof(status));
    }
    reply.index = applied;
    reply.flags = count - applied;
    return ManagedBridgeStatus::OK;
}

} /// @note namespace fbzz::scene::managed_detail
