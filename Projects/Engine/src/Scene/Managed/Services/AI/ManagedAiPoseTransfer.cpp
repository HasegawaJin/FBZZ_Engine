/// @file    ManagedAiPoseTransfer.cpp
/// @brief   Validate borrowed AI pose packets and stage hierarchy math before publication.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include "ManagedAiPoseTransfer.hpp"
#include "../../../Systems/Transform/WorldPoseHelpers.hpp"
#include <Engine/Profiler/ProfileScope.hpp>
#include "Engine/Scene/Components/Physics/RigidBodyComponent.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Systems/Transform/TransformSystem.hpp"
#include "Math/Scalar/MathUtils.hpp"
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
/// @note Math の契約を呼び出す前に、Quaternion の正規化と逆元の前提条件を確認する。
/// @see Projects/Math/src/Quaternion.cpp Quaternion::Normalized / Quaternion::Inverse.
bool Normalizable(const math::Quaternion& value, float& length)
{
    length = value.Length();
    return Finite(value) && std::isfinite(length) && !math::NearlyZero(length);
}
bool Normalizable(const math::Quaternion& value)
{
    float length = 0.0f;
    return Normalizable(value, length);
}
/// @pre 渡された長さは、コールバックを呼ばない同じ演算段階で Normalizable を満たした値であること。
math::Quaternion NormalizeValidated(const math::Quaternion& value, float length)
{
    const float inverse = 1.0f / length;
    return {value.x * inverse, value.y * inverse, value.z * inverse, value.w * inverse};
}
bool Invertible(const math::Quaternion& value, float& squared)
{
    squared = value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
    return Finite(value) && std::isfinite(squared) && !math::NearlyZero(squared);
}
/// @pre 渡された二乗長は Invertible を満たすこと。Quaternion::Inverse と同じ順で符号を反転する。
math::Quaternion InverseValidated(const math::Quaternion& value, float squared)
{
    const float inverse = 1.0f / squared;
    return {-value.x * inverse, -value.y * inverse, -value.z * inverse, value.w * inverse};
}
/// @note 符号付きゼロを含め、ゼロの厳密一致を使う物理用関数ではなく SetWorldPose の許容値に揃える。
math::Vector3 DividePoseScale(const math::Vector3& value, const math::Vector3& scale)
{
    return {std::abs(scale.x) > 1e-6f ? value.x / scale.x : 0.0f,
        std::abs(scale.y) > 1e-6f ? value.y / scale.y : 0.0f,
        std::abs(scale.z) > 1e-6f ? value.z / scale.z : 0.0f};
}
template<typename T> T Read(const char* bytes)
{
    T result{};
    std::memcpy(&result, bytes, sizeof(result));
    return result;
}
}

void ManagedAiPoseTransfer::BeginEpoch(const Scene& scene)
{
    m_publish.clear();
    m_roots.clear();
    m_ancestorOrder.clear();
    m_queue.clear();
    const size_t slots = scene.EntitySlotCount();
    m_shadow.resize(slots);
    world_pose_detail::ReserveAiWorldMath(slots);
    m_cloneEpoch.resize(slots);
    m_targetEpoch.resize(slots);
    m_ancestorEpoch.resize(slots);
    m_pathEpoch.resize(slots);
    m_subtreeEpoch.resize(slots);
    m_subtreeBegin.resize(slots);
    m_subtreeEnd.resize(slots);
    if (++m_epoch == 0) {
        std::fill(m_cloneEpoch.begin(), m_cloneEpoch.end(), 0);
        std::fill(m_targetEpoch.begin(), m_targetEpoch.end(), 0);
        std::fill(m_ancestorEpoch.begin(), m_ancestorEpoch.end(), 0);
        std::fill(m_pathEpoch.begin(), m_pathEpoch.end(), 0);
        std::fill(m_subtreeEpoch.begin(), m_subtreeEpoch.end(), 0);
        m_epoch = 1;
    }
}

Transform* ManagedAiPoseTransfer::Shadow(GameObject& object)
{
    const uint32_t index = object.GetID().index;
    if (index >= m_shadow.size()) {
        return nullptr;
    }
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
    if (!transform || (parent && !parentTransform)) {
        return false;
    }
    if (world_pose_detail::TryReuseAiWorldMath(object.GetID().index, *transform, parentTransform)) {
        return true;
    }
    if (!Finite(transform->position)
        || !Finite(transform->scale) || !Normalizable(transform->rotation)) {
        return false;
    }
    // 保持した階層順は、検証済みの祖先を子より先に処理できる。コールバックの呼び出しと対象の入れ子はない。
    // Parent world finiteness and quaternion normalizability were already established by that successful Update.
    math::Quaternion worldRotation;
    float rotationLength = 0.0f;
    if (parentTransform) {
        worldRotation = parentTransform->worldRotation * transform->rotation;
        rotationLength = worldRotation.Length();
        if (!Finite(worldRotation) || !std::isfinite(rotationLength) || math::NearlyZero(rotationLength)) {
            return false;
        }
    }
    if (parentTransform) {
        // 検証済みの積と長さを使い、Quaternion::Normalized と同じ逆数計算・乗算の順を保つ。
        /// @see Projects/Math/src/Quaternion.cpp Quaternion::Normalized.
        transform->worldRotation = NormalizeValidated(worldRotation, rotationLength);
        const math::Vector3 scaledLocal = {transform->position.x * parentTransform->worldScale.x,
            transform->position.y * parentTransform->worldScale.y, transform->position.z * parentTransform->worldScale.z};
        transform->worldPosition = parentTransform->worldPosition + parentTransform->worldRotation * scaledLocal;
        transform->worldScale = {parentTransform->worldScale.x * transform->scale.x,
            parentTransform->worldScale.y * transform->scale.y, parentTransform->worldScale.z * transform->scale.z};
    } else {
        UpdateWorldTransform(*transform, nullptr);
    }
    // UpdateWorldTransform はワールド値だけを書き込む。ローカル入力はこの前に検証済み。
    if (!Finite(transform->worldPosition) || !Finite(transform->worldRotation)
        || !Finite(transform->worldScale)) {
        return false;
    }
    world_pose_detail::RememberAiWorldMath(object.GetID().index, *transform, parentTransform);
    return true;
}

bool ManagedAiPoseTransfer::ValidateHierarchy(const Scene& scene)
{
    const size_t slots = scene.EntitySlotCount();
    // 階層の検証結果は、解決関数の呼び出しと全対象への印付けが終わってから保持する。
    // 検証済みの祖先経路に対象が含まれないため、後続の経路は検証済みの点で打ち切れる。
    for (const Entry& entry : m_entries) {
        if (!entry.object) {
            continue;
        }
        const uint32_t target = entry.object->GetID().index;
        if (m_subtreeEpoch[target] == m_epoch) {
            continue;
        }
        m_subtreeEpoch[target] = m_epoch;
        m_roots.push_back(entry.object);
        m_ancestors.clear();
        for (GameObject* parent = entry.object->GetParent(); parent; parent = parent->GetParent()) {
            const uint32_t index = parent->GetID().index;
            if (parent->GetScene() != &scene || scene.GetGameObject(parent->GetID()) != parent
                || index >= m_targetEpoch.size() || m_targetEpoch[index] == m_epoch) {
                return false;
            }
            if (m_ancestorEpoch[index] == m_epoch) {
                break;
            }
            if (m_pathEpoch[index] == m_epoch || m_ancestors.size() >= slots) {
                return false;
            }
            m_pathEpoch[index] = m_epoch;
            m_ancestors.push_back(parent);
        }
        for (auto it = m_ancestors.rbegin(); it != m_ancestors.rend(); ++it) {
            m_ancestorOrder.push_back(*it);
            m_ancestorEpoch[(*it)->GetID().index] = m_epoch;
        }
    }
    // 祖先と子孫がともに対象である場合を拒否するので、別の対象の部分木は重ならない。
    // 非アクティブな子孫も含めて幅優先順の各範囲を 1 回だけ保持する。この段階以降にコールバックはない。
    for (GameObject* root : m_roots) {
        const uint32_t slot = root->GetID().index;
        const size_t begin = m_queue.size();
        if (begin >= slots) {
            return false;
        }
        m_queue.push_back(root);
        for (size_t index = begin; index < m_queue.size(); ++index) {
            GameObject& object = *m_queue[index];
            if (object.GetScene() != &scene || scene.GetGameObject(object.GetID()) != &object) {
                return false;
            }
            for (int childIndex = 0; childIndex < object.GetChildCount(); ++childIndex) {
                if (GameObject* child = object.GetChild(childIndex)) {
                    if (m_queue.size() >= slots || child->GetParent() != &object) {
                        return false;
                    }
                    m_queue.push_back(child);
                }
            }
        }
        m_subtreeBegin[slot] = begin;
        m_subtreeEnd[slot] = m_queue.size();
    }
    return true;
}

bool ManagedAiPoseTransfer::Stage(Entry& entry)
{
    GameObject& object = *entry.object;
    const uint32_t slot = object.GetID().index;
    const size_t begin = m_subtreeBegin[slot], end = m_subtreeEnd[slot];
    // 同じ対象が複数回現れる場合も、途中の適用前後の演算をパケット順にすべて実行する。
    // 後続の同一対象が、先行したオーバーフローや無効な子孫を隠すことはできない。
    for (size_t index = begin; index < end; ++index)
        if (!Update(*m_queue[index])) {
            return false;
        }
    Transform* transform = Shadow(object);
    GameObject* parent = object.GetParent();
    const Transform* parentTransform = parent ? Shadow(*parent) : nullptr;
    if (!transform || (parent && !parentTransform)) {
        return false;
    }
    math::Vector3 parentSpace;
    math::Quaternion localRotation;
    float localRotationLength = 0.0f;
    if (parentTransform) {
        float squared = 0.0f;
        if (!Invertible(parentTransform->worldRotation, squared)) {
            return false;
        }
        const math::Quaternion inverse = InverseValidated(parentTransform->worldRotation, squared);
        const math::Vector3 offset = entry.position - parentTransform->worldPosition;
        if (!Finite(inverse) || !Finite(offset)) {
            return false;
        }
        parentSpace = inverse * offset;
        if (!Finite(parentSpace)) {
            return false;
        }
        localRotation = inverse * entry.rotation;
        if (!Normalizable(localRotation, localRotationLength)) {
            return false;
        }
    }
    // 後続の解決関数で浮動小数点環境が変わり得るため、解析済み行の長さを呼び出しの前後で使い回さない。
    // この段階はコールバックを呼ばないので、SetWorldPose の再計算を検証済みの係数で厳密に置き換えられる。
    float worldRotationLength = 0.0f;
    if (!Normalizable(entry.rotation, worldRotationLength)) {
        return false;
    }
    const math::Quaternion worldRotation = NormalizeValidated(entry.rotation, worldRotationLength);
    const math::Vector3 localScale = transform->scale, worldScale = transform->worldScale;
    transform->worldPosition = entry.position;
    transform->worldRotation = worldRotation;
    transform->worldScale = worldScale;
    if (parentTransform) {
        transform->position = DividePoseScale(parentSpace, parentTransform->worldScale);
        transform->rotation = NormalizeValidated(localRotation, localRotationLength);
        transform->scale = DividePoseScale(worldScale, parentTransform->worldScale);
    } else {
        transform->position = entry.position;
        transform->rotation = worldRotation;
        transform->scale = worldScale;
    }
    // 元のローカルスケールを戻す前に、投影した一時スケールに対して SetWorldPose と同じ有限性チェックを行う。
    if (!Finite(*transform)) {
        return false;
    }
    transform->scale = localScale;
    for (size_t index = begin; index < end; ++index)
        if (!Update(*m_queue[index])) {
            return false;
        }
    return true;
}

ManagedBridgeStatus ManagedAiPoseTransfer::Run(Scene& scene, const ManagedBridgeRequest& request,
    ManagedBridgeReply& reply, ObjectResolver resolver, void* resolverState)
{
    FBZZ_PROFILE_SCOPE("LateScriptSystem::NativePoseTransfer");
    reply.index = reply.flags = reply.outputBytes = 0;
    // この操作が使わない要求フィールドはゼロでなければならない。
    if (request.op != ManagedBridgeOp::AI_CROWD_STEP || request.index != 1 || request.flags || !resolver) {
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    if (request.reserved0 || request.reserved1 || request.x != 0 || request.y != 0 || request.z != 0 || request.w != 0) {
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    }

    // 要素数とバッファが有効な場合にだけ、入力ヘッダーを参照する。
    if (request.integer < 0 || request.integer > MAX_AGENTS) {
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    if (!request.input || (request.outputCapacity && !request.output)) {
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    const uint32_t count = static_cast<uint32_t>(request.integer);
    if (request.inputBytes != HEADER_BYTES + count * ROW_BYTES) {
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    if (Read<uint32_t>(request.input) != count) {
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    }

    // 容量照会はシーンに触れず、応答に必要なサイズだけを設定する。
    const uint32_t outputBytes = count * STATUS_BYTES;
    reply.outputBytes = outputBytes;
    if (request.outputCapacity < outputBytes) {
        return ManagedBridgeStatus::BUFFER_TOO_SMALL;
    }
    // ステータスの書き込み先が入力と重複し、呼び出し元の入力スナップショットを壊さないようにする。
    const std::less<const char*> before;
    if (outputBytes && before(request.input, request.output + outputBytes)
        && before(request.output, request.input + request.inputBytes)) {
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    // 全行の対象と姿勢を解決し、階層を検証してから作業領域へ反映する。
    BeginEpoch(scene);
    m_entries.resize(count);
    const uint64_t lifetime = scene.GetRenderSceneGeneration();
    uint32_t applied = 0;
    for (uint32_t index = 0; index < count; ++index) {
        Entry& entry = m_entries[index];
        entry = Entry{};
        const char* row = request.input + HEADER_BYTES + index * ROW_BYTES;
        const uint64_t id = Read<uint64_t>(row), rowLifetime = Read<uint64_t>(row + 8);
        if (!id && !rowLifetime) {
            continue;
        }
        if (rowLifetime != lifetime) {
            return ManagedBridgeStatus::SCENE_MISMATCH;
        }
        entry.status = ManagedObjectStatus::EXPIRED;
        if (!id) {
            continue;
        }
        const ManagedBridgeStatus status = resolver(resolverState, id, entry.object);
        if (status != ManagedBridgeStatus::OK) {
            return status;
        }
        if (!entry.object) {
            continue;
        }
        const uint32_t slot = entry.object->GetID().index;
        if (entry.object->GetScene() != &scene) {
            return ManagedBridgeStatus::SCENE_MISMATCH;
        }
        if (scene.GetGameObject(entry.object->GetID()) != entry.object || slot >= m_shadow.size()) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const auto* body = entry.object->GetComponent<RigidBodyComponent>();
        // 明示的に設定された静的ボディは PhysicsSystem で Transform から同期し、重力は積分しない。
        // 動的ボディと通常の静的ボディは Physics が排他的に管理する。対象に含まれればパケット全体を拒否する。
        if (body && body->enabled && body->rigidBody
            && (!body->poseDriven || !body->rigidBody->IsStatic())) {
            return ManagedBridgeStatus::PHYSICS_BODY;
        }
        entry.position = {Read<float>(row + 16), Read<float>(row + 20), Read<float>(row + 24)};
        entry.rotation = {Read<float>(row + 28), Read<float>(row + 32), Read<float>(row + 36), Read<float>(row + 40)};
        float rotationLength = 0.0f;
        if (!Finite(entry.position) || !Normalizable(entry.rotation, rotationLength)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        entry.rotation = NormalizeValidated(entry.rotation, rotationLength);
        if (!Normalizable(entry.rotation)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        m_targetEpoch[slot] = m_epoch;
        entry.status = ManagedObjectStatus::VALID;
        ++applied;
    }
    world_pose_detail::PrepareAiWorldMath();
    if (!ValidateHierarchy(scene)) {
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    for (GameObject* ancestor : m_ancestorOrder)
        if (!Update(*ancestor)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
    for (Entry& entry : m_entries)
        if (entry.object && !Stage(entry)) {
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
    // 検証後にコールバックや解決関数の呼び出しはなく、準備段階の階層計算はすべて成功している。
    // 確定した作業領域には、検証済みの祖先と子孫のワールド姿勢がすべて入っている。
    // その値を一度だけ公開する。階層を三度目に計算しても同じ演算を繰り返すことになる。
    for (GameObject* object : m_publish) {
        object->transform = m_shadow[object->GetID().index];
    }
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t status = static_cast<uint32_t>(m_entries[index].status);
        std::memcpy(request.output + index * STATUS_BYTES, &status, sizeof(status));
    }
    reply.index = applied;
    reply.flags = count - applied;
    return ManagedBridgeStatus::OK;
}

}
