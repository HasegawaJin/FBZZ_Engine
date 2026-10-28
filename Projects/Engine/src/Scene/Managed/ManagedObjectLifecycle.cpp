/// @file    ManagedObjectLifecycle.cpp
/// @brief   生成前の容量・姿勢検証と破棄要求の全件検証を行う。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include "ManagedObjectLifecycle.hpp"
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Math/MathUtils.hpp>
#include <toml++/toml.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::scene::managed_detail {
namespace {

/// @see https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-multibytetowidechar MB_ERR_INVALID_CHARS。
bool ValidText(std::string_view value)
{
    return !value.empty() && value.size() <= static_cast<size_t>(INT_MAX) && value.find('\0') == std::string_view::npos
        && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0) > 0;
}

bool PreparePose(const math::Vector3* position, const math::Quaternion* rotation, math::Quaternion& normalized)
{
    if (!position || !rotation) return !position && !rotation;
    if (!std::isfinite(position->x) || !std::isfinite(position->y) || !std::isfinite(position->z)
        || !std::isfinite(rotation->x) || !std::isfinite(rotation->y)
        || !std::isfinite(rotation->z) || !std::isfinite(rotation->w)) return false;
    const float length = rotation->Length();
    if (!std::isfinite(length) || math::NearlyZero(length)) return false;
    normalized = rotation->Normalized();
    return true;
}

void ApplyPose(GameObject& object, const math::Vector3* position, const math::Quaternion& rotation)
{
    if (position) {
        object.transform.position = *position;
        object.transform.rotation = rotation;
    }
    FlushWorldTransforms(object);
}

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

/// @note 子孫の正規化を実行する前に MathContract の事前条件も確認する。
/// @see Projects/Math/src/Quaternion.cpp Quaternion::Normalized。
bool FiniteNormalization(const math::Quaternion& value)
{
    if (!std::isfinite(value.x) || !std::isfinite(value.y)
        || !std::isfinite(value.z) || !std::isfinite(value.w)) return false;
    const float length = value.Length();
    return std::isfinite(length) && !math::NearlyZero(length);
}

/// @note 有限な pose でも親の scale と子孫の合成で overflow するため、全階層の検証後だけ公開する。
bool ApplyPrefabPose(GameObject& root, const math::Vector3* position, const math::Quaternion& rotation,
    const std::vector<uint32_t>& previous, std::vector<uint32_t>* accepted = nullptr)
{
    if (root.GetParent() || !root.GetScene()) return false;
    const size_t slots = root.GetScene()->EntitySlotCount();
    struct Prepared {
        GameObject* object;
        Transform transform;
        size_t parent;
    };
    std::vector<Prepared> prepared;
    prepared.push_back({&root, root.transform, EntityID::INVALID_INDEX});
    if (position) {
        prepared.front().transform.position = *position;
        prepared.front().transform.rotation = rotation;
    }
    std::unordered_set<uint32_t> seen;
    for (size_t index = 0; index < prepared.size(); ++index) {
        GameObject& object = *prepared[index].object;
        const EntityID entity = object.GetID();
        if (entity.index >= slots || !seen.insert(entity.index).second
            || (entity.index < previous.size() && previous[entity.index] == entity.generation)
            || object.GetScene() != root.GetScene() || root.GetScene()->GetGameObject(entity) != &object) return false;
        Transform& transform = prepared[index].transform;
        const Transform* parent = index == 0 ? nullptr : &prepared[prepared[index].parent].transform;
        if (!Finite(transform.position) || !Finite(transform.scale) || !FiniteNormalization(transform.rotation)
            || (parent && (!FiniteNormalization(parent->worldRotation * transform.rotation)))) return false;
        UpdateWorldTransform(transform, parent);
        if (!Finite(transform.worldPosition) || !Finite(transform.worldScale)
            || !FiniteNormalization(transform.worldRotation)) return false;
        for (int childIndex = 0; childIndex < object.GetChildCount(); ++childIndex) {
            GameObject* child = object.GetChild(childIndex);
            if (!child || child->GetParent() != &object || prepared.size() >= slots) return false;
            prepared.push_back({child, child->transform, index});
        }
    }
    for (const Prepared& entry : prepared) entry.object->transform = entry.transform;
    if (accepted) {
        accepted->resize(root.GetScene()->EntitySlotCount());
        for (const Prepared& entry : prepared) {
            const EntityID entity = entry.object->GetID();
            (*accepted)[entity.index] = entity.generation;
        }
    }
    return true;
}

struct ExistingObject {
    EntityID entity;
    EntityID parent;
    int sibling;
    Transform transform;
};

void SnapshotExisting(Scene& scene, std::vector<uint32_t>& generations,
    std::vector<ExistingObject>& originals)
{
    generations.resize(scene.EntitySlotCount());
    originals.reserve(scene.GameObjectCount());
    std::unordered_map<uint32_t, int> childSiblings;
    childSiblings.reserve(scene.GameObjectCount());
    for (GameObject& object : scene.GameObjects())
        for (int childIndex = 0; childIndex < object.GetChildCount(); ++childIndex)
            if (GameObject* child = object.GetChild(childIndex)) childSiblings.emplace(child->GetID().index, childIndex);
    int rootSibling = 0;
    for (GameObject& object : scene.GameObjects()) {
        const EntityID entity = object.GetID();
        generations[entity.index] = entity.generation;
        GameObject* parent = object.GetParent();
        const auto childSibling = childSiblings.find(entity.index);
        const int sibling = parent ? (childSibling != childSiblings.end() ? childSibling->second : 0) : rootSibling++;
        originals.push_back({entity, parent ? parent->GetID() : EntityID::INVALID, sibling, object.transform});
    }
}

/// @note 注入された生成経路が既存 GO を新規階層へ付けても、再帰破棄の前に既存実体を保護する。
void RollbackAdded(Scene& scene, const std::vector<uint32_t>& previous,
    const std::vector<ExistingObject>& originals)
{
    for (const ExistingObject& original : originals)
        if (GameObject* object = scene.GetGameObject(original.entity)) object->ClearParent();
    std::vector<EntityID> added;
    for (GameObject& object : scene.GameObjects())
        if (object.GetID().index >= previous.size() || previous[object.GetID().index] != object.GetID().generation)
            added.push_back(object.GetID());
    scene.DestroyGameObjects(added);
    for (const ExistingObject& original : originals)
        if (GameObject* object = scene.GetGameObject(original.entity))
            object->SetParent(scene.GetGameObject(original.parent));
    std::vector<const ExistingObject*> siblings;
    siblings.reserve(originals.size());
    for (const ExistingObject& original : originals) siblings.push_back(&original);
    std::sort(siblings.begin(), siblings.end(), [](const ExistingObject* left, const ExistingObject* right) {
        if (left->parent.index != right->parent.index) return left->parent.index < right->parent.index;
        return left->sibling < right->sibling;
    });
    for (const ExistingObject* original : siblings)
        if (GameObject* object = scene.GetGameObject(original->entity)) object->SetSiblingIndex(original->sibling);
    for (size_t index = 0; index < originals.size(); ++index) {
        const ExistingObject& original = originals[index];
        if (GameObject* object = scene.GetGameObject(original.entity)) {
            scene.MoveGameObjectToIndex(original.entity, index);
            object->transform = original.transform;
        }
    }
}

ManagedBridgeStatus InspectPrefab(Scene& scene, const std::string& reference, size_t* entityCount = nullptr)
{
    const std::string path = ResolvePrefabAssetPath(reference);
    std::string text;
    if (path.empty() || !util::FileSystem::ReadText(path, text)) return ManagedBridgeStatus::NOT_FOUND;
    const toml::parse_result document = toml::parse(text);
    if (!document) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const toml::array* objects = document.table()["gameobjects"].as_array();
    if (!objects || objects->empty())
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    size_t roots = 0;
    for (const toml::node& item : *objects) {
        const toml::table* object = item.as_table();
        if (!object) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if ((*object)["parentInstanceId"].value_or(std::string{}).empty()
            && (*object)["parent"].value_or(std::string{}).empty()) ++roots;
    }
    if (roots != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (entityCount) *entityCount = objects->size();
    return scene.CanCreateGameObjects(objects->size()) ? ManagedBridgeStatus::OK : ManagedBridgeStatus::CAPACITY_LIMIT;
}

ManagedBridgeStatus ValidateTarget(Scene& scene, const GameObject& target)
{
    if (target.GetScene() != &scene) return ManagedBridgeStatus::SCENE_MISMATCH;
    return scene.GetGameObject(target.GetID()) == &target ? ManagedBridgeStatus::OK : ManagedBridgeStatus::NOT_FOUND;
}

} /// @note namespace

ManagedBridgeStatus ManagedObjectLifecycle::Create(Scene& scene, std::string_view name, GameObject*& created,
    const math::Vector3* position, const math::Quaternion* rotation)
{
    math::Quaternion normalized;
    if (!ValidText(name) || !PreparePose(position, rotation, normalized)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    GameObject* object = scene.TryCreateGameObject(std::string(name));
    if (!object) return ManagedBridgeStatus::CAPACITY_LIMIT;
    ApplyPose(*object, position, normalized);
    created = object;
    return ManagedBridgeStatus::OK;
}

ManagedBridgeStatus ManagedObjectLifecycle::Instantiate(Scene& scene, std::string_view reference, GameObject*& created,
    const math::Vector3* position, const math::Quaternion* rotation)
{
    math::Quaternion normalized;
    if (!ValidText(reference) || !PreparePose(position, rotation, normalized)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const std::string prefab(reference);
    const ManagedBridgeStatus inspected = InspectPrefab(scene, prefab);
    if (inspected != ManagedBridgeStatus::OK) return inspected;
    std::vector<uint32_t> previous;
    std::vector<ExistingObject> originals;
    SnapshotExisting(scene, previous, originals);
    std::vector<EntityID> roots;
    if (!Script::InstantiatePrefab(scene, prefab, roots) || roots.size() != 1 || !scene.GetGameObject(roots.front())) {
        /// @note 読取後のアセット変更や注入された生成経路の失敗でも、この操作で追加された実体だけを戻す。
        RollbackAdded(scene, previous, originals);
        return ManagedBridgeStatus::LOAD_FAILED;
    }
    GameObject* root = scene.GetGameObject(roots.front());
    if (!ApplyPrefabPose(*root, position, normalized, previous)) {
        RollbackAdded(scene, previous, originals);
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    created = root;
    return ManagedBridgeStatus::OK;
}

ManagedBridgeStatus ManagedObjectLifecycle::SpawnMany(Scene& scene, std::string_view reference, bool prefab,
    std::span<const SpawnPose> poses, std::vector<GameObject*>& created)
{
    if (!ValidText(reference)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (poses.empty()) {
        created.clear();
        return ManagedBridgeStatus::OK;
    }
    std::vector<math::Quaternion> rotations;
    rotations.reserve(poses.size());
    for (const SpawnPose& pose : poses) {
        math::Quaternion normalized;
        if (!PreparePose(&pose.position, &pose.rotation, normalized)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        rotations.push_back(normalized);
    }
    size_t entities = 1;
    if (prefab && !poses.empty()) {
        const ManagedBridgeStatus inspected = InspectPrefab(scene, std::string(reference), &entities);
        if (inspected != ManagedBridgeStatus::OK) return inspected;
    }
    if (poses.size() > (std::numeric_limits<size_t>::max)() / entities
        || !scene.CanCreateGameObjects(entities * poses.size())) return ManagedBridgeStatus::CAPACITY_LIMIT;
    std::vector<uint32_t> previous;
    std::vector<ExistingObject> originals;
    SnapshotExisting(scene, previous, originals);
    std::vector<uint32_t> protectedEntities = previous;
    PrefabInstantiationBatch batch(scene);
    std::vector<GameObject*> prepared;
    prepared.reserve(poses.size());
    for (size_t index = 0; index < poses.size(); ++index) {
        const SpawnPose& pose = poses[index];
        GameObject* object = nullptr;
        ManagedBridgeStatus status = ManagedBridgeStatus::OK;
        if (prefab) {
            if (Script::HasPrefabInstantiationCallback()) {
                batch.DisableSceneCache();
                status = InspectPrefab(scene, std::string(reference));
            }
            if (status == ManagedBridgeStatus::OK) {
                std::vector<EntityID> roots;
                if (!Script::InstantiatePrefab(scene, std::string(reference), roots) || roots.size() != 1
                    || !(object = scene.GetGameObject(roots.front()))) status = ManagedBridgeStatus::LOAD_FAILED;
                else if (!ApplyPrefabPose(*object, &pose.position, rotations[index], protectedEntities, &protectedEntities))
                    status = ManagedBridgeStatus::INVALID_ARGUMENT;
                else if (!batch.CanReuseSceneCache()) {
                    /// @note 任意 callback の補助実体も、次の生成で既存階層へ取り込ませない。
                    protectedEntities.resize(scene.EntitySlotCount());
                    for (GameObject& existing : scene.GameObjects())
                        protectedEntities[existing.GetID().index] = existing.GetID().generation;
                }
            }
        } else {
            object = scene.TryCreateGameObject(std::string(reference));
            if (!object) status = ManagedBridgeStatus::CAPACITY_LIMIT;
            else ApplyPose(*object, &pose.position, rotations[index]);
        }
        if (status != ManagedBridgeStatus::OK || !object) {
            RollbackAdded(scene, previous, originals);
            return status == ManagedBridgeStatus::OK ? ManagedBridgeStatus::LOAD_FAILED : status;
        }
        prepared.push_back(object);
    }
    created = std::move(prepared);
    return ManagedBridgeStatus::OK;
}

ManagedBridgeStatus ManagedObjectLifecycle::Destroy(Scene& scene, GameObject& target, float delay)
{
    if (!std::isfinite(delay) || delay < 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const ManagedBridgeStatus valid = ValidateTarget(scene, target);
    if (valid != ManagedBridgeStatus::OK) return valid;
    GameObject::Destroy(target, delay);
    return ManagedBridgeStatus::OK;
}

ManagedBridgeStatus ManagedObjectLifecycle::DestroyMany(Scene& scene, std::span<GameObject* const> targets,
    float delay, uint32_t& applied, uint32_t& skipped, bool publish)
{
    applied = skipped = 0;
    if (targets.size() > UINT32_MAX || !std::isfinite(delay) || delay < 0)
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    std::vector<bool> seen(scene.EntitySlotCount());
    std::vector<GameObject*> unique;
    unique.reserve(std::min(targets.size(), scene.GameObjectCount()));
    for (GameObject* target : targets) {
        if (!target) continue;
        const ManagedBridgeStatus valid = ValidateTarget(scene, *target);
        if (valid != ManagedBridgeStatus::OK) return valid;
        const uint32_t index = target->GetID().index;
        if (seen[index]) continue;
        seen[index] = true;
        unique.push_back(target);
    }
    if (publish) for (GameObject* target : unique) GameObject::Destroy(*target, delay);
    applied = static_cast<uint32_t>(unique.size());
    skipped = static_cast<uint32_t>(targets.size()) - applied;
    return ManagedBridgeStatus::OK;
}

} /// @note namespace fbzz::scene::managed_detail
