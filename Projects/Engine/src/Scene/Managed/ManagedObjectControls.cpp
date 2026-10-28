/// @file    ManagedObjectControls.cpp
/// @brief   寿命検証後の Component 操作と循環検出付きの階層検索。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include "ManagedObjectControls.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/VFXComponent.hpp>
#include <Engine/Scene/Components/SequencePlayerComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/JointComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/MotionWarpComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Engine/Scene/Components/SpringBoneComponent.hpp>
#include <Engine/Scene/Components/UIAnimator.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/AudioSpatialComponents.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <cmath>
#include <vector>

namespace fbzz::scene::managed_detail {
namespace {
bool Text(std::string_view value) { return !value.empty() && value.size() <= 1024U * 1024U && value.find('\0') == std::string_view::npos; }
template<typename T>
ManagedBridgeStatus ComponentAccess(GameObject& target, ManagedBridgeOp op, bool& exists, uint64_t* lifetime)
{
    exists = target.GetComponent<T>() != nullptr;
    if (op == ManagedBridgeOp::COMPONENT_ADD) {
        if (!exists) target.AddComponent<T>();
        exists = true;
    } else if (op == ManagedBridgeOp::COMPONENT_REMOVE) {
        if (exists) target.RemoveComponent<T>();
    } else if (op != ManagedBridgeOp::COMPONENT_READ) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (lifetime && exists && target.GetScene())
        *lifetime = target.GetScene()->GetComponentLifetime<T>(target.GetID());
    return ManagedBridgeStatus::OK;
}
} /// @note namespace

ManagedBridgeStatus ManagedObjectControls::Read(GameObject& target, uint32_t selector, ManagedBridgeReply& reply, std::string& text)
{
    text.clear();
    switch (selector) {
    case 0: reply.integer = target.activeSelf(); break;
    case 1: reply.integer = target.activeInHierarchy(); break;
    case 2: reply.integer = target.layer; break;
    case 3: text = target.name; break;
    case 4: text = target.tag; break;
    default: return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    return ManagedBridgeStatus::OK;
}

ManagedBridgeStatus ManagedObjectControls::Write(GameObject& target, uint32_t selector, int64_t value, std::string_view text)
{
    switch (selector) {
    case 0:
        if (value != 0 && value != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        target.SetActive(value != 0); break;
    case 2:
        if (value < 0 || value >= 32) return ManagedBridgeStatus::INVALID_ARGUMENT;
        target.layer = static_cast<int>(value); break;
    case 3:
        if (!Text(text)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        target.name.assign(text); break;
    case 4:
        if (!Text(text)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        target.tag.assign(text); break;
    default: return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    return ManagedBridgeStatus::OK;
}

ManagedBridgeStatus ManagedObjectControls::Component(GameObject& target, uint32_t kind, ManagedBridgeOp op, bool& exists, uint64_t* lifetime)
{
    exists = false;
    if (lifetime) *lifetime = 0;
    if (kind == 7) return ManagedBridgeStatus::UNSUPPORTED;
    if (kind >= target.m_pendingManagedComponentRemovals.size()) return ManagedBridgeStatus::UNSUPPORTED;
    if (target.m_pendingManagedComponentRemovals.test(kind)) {
        if (op == ManagedBridgeOp::COMPONENT_READ) return ManagedBridgeStatus::OK;
        if (op == ManagedBridgeOp::COMPONENT_ADD) return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    if (kind > 0 && op == ManagedBridgeOp::COMPONENT_REMOVE) {
        const auto status = Component(target, kind, ManagedBridgeOp::COMPONENT_READ, exists);
        if (status == ManagedBridgeStatus::OK && exists) target.m_pendingManagedComponentRemovals.set(kind);
        return status;
    }
    switch (kind) {
    case 0: exists = true; return op == ManagedBridgeOp::COMPONENT_REMOVE ? ManagedBridgeStatus::UNSUPPORTED : ManagedBridgeStatus::OK;
    case 1: {
        if (op == ManagedBridgeOp::COMPONENT_ADD) {
            auto* component = target.GetComponent<RigidBodyComponent>();
            if (!component || !component->rigidBody) {
                const auto& pose = target.transform;
                const float length = pose.worldRotation.Length();
                if (!std::isfinite(pose.worldPosition.x) || !std::isfinite(pose.worldPosition.y) || !std::isfinite(pose.worldPosition.z)
                    || !std::isfinite(pose.worldRotation.x) || !std::isfinite(pose.worldRotation.y) || !std::isfinite(pose.worldRotation.z)
                    || !std::isfinite(pose.worldRotation.w) || !std::isfinite(length) || length < math::EPSILON)
                    return ManagedBridgeStatus::INVALID_ARGUMENT;
                /// @note Component 追加と同じ呼出で facade を公開する。World の登録は既存 PrePhysics 同期が行う。
                auto body = std::make_unique<physics::RigidBody>();
                body->SetPosition(pose.worldPosition); body->SetRotation(pose.worldRotation);
                if (!body->IsFiniteState()) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (!component) component = &target.AddComponent<RigidBodyComponent>();
                component->rigidBody = std::move(body);
            }
        }
        /// @note authored row の寿命は Physics 実体の初期化から独立する。Physics 操作は各 service が実体を検証する。
        return ComponentAccess<RigidBodyComponent>(target, op, exists, lifetime);
    }
    case 2: return ComponentAccess<BoxColliderComponent>(target, op, exists, lifetime);
    case 3: return ComponentAccess<SphereColliderComponent>(target, op, exists, lifetime);
    case 4: return ComponentAccess<CapsuleColliderComponent>(target, op, exists, lifetime);
    case 5: return ComponentAccess<AnimatorComponent>(target, op, exists, lifetime);
    case 6: return ComponentAccess<AudioSourceComponent>(target, op, exists, lifetime);
    case 7:
        exists = target.GetComponent<UIText>() || target.GetComponent<UIImage>() || target.GetComponent<UIButton>() || target.GetComponent<UICanvas>()
            || target.GetComponent<UISlider>() || target.GetComponent<UIToggle>() || target.GetComponent<UIScrollView>()
            || target.GetComponent<UIInputField>() || target.GetComponent<UICanvasGroup>() || target.GetComponent<UIDragSource>() || target.GetComponent<UIDropTarget>();
        return op == ManagedBridgeOp::COMPONENT_READ ? ManagedBridgeStatus::OK : ManagedBridgeStatus::UNSUPPORTED;
    case 8: return ComponentAccess<CameraComponent>(target, op, exists, lifetime);
    case 9: return ComponentAccess<NavMeshAgentComponent>(target, op, exists, lifetime);
    case 10: return ComponentAccess<CharacterControllerComponent>(target, op, exists, lifetime);
    case 11: return ComponentAccess<UIText>(target, op, exists, lifetime);
    case 12: return ComponentAccess<UIImage>(target, op, exists, lifetime);
    case 13: return ComponentAccess<UIButton>(target, op, exists, lifetime);
    case 14: return ComponentAccess<UICanvas>(target, op, exists, lifetime);
    case 15: return ComponentAccess<CylinderColliderComponent>(target, op, exists, lifetime);
    case 16: return ComponentAccess<AabbColliderComponent>(target, op, exists, lifetime);
    case 17: return ComponentAccess<MeshColliderComponent>(target, op, exists, lifetime);
    case 18: return ComponentAccess<ConvexHullColliderComponent>(target, op, exists, lifetime);
    case 19: return ComponentAccess<TerrainColliderComponent>(target, op, exists, lifetime);
    case 20: return ComponentAccess<UISlider>(target, op, exists, lifetime);
    case 21: return ComponentAccess<UIToggle>(target, op, exists, lifetime);
    case 22: return ComponentAccess<UIScrollView>(target, op, exists, lifetime);
    case 23: return ComponentAccess<UIInputField>(target, op, exists, lifetime);
    case 24: return ComponentAccess<UICanvasGroup>(target, op, exists, lifetime);
    case 25: return ComponentAccess<UIDragSource>(target, op, exists, lifetime);
    case 26: return ComponentAccess<UIDropTarget>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::LIGHT): return ComponentAccess<LightComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::PARTICLE_SYSTEM): return ComponentAccess<ParticleEmitter>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::MATERIAL): return ComponentAccess<MaterialComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::VFX): return ComponentAccess<VFXComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::SEQUENCE): return ComponentAccess<SequencePlayerComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::TERRAIN): return ComponentAccess<TerrainComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::WATER): return ComponentAccess<WaterComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::JOINT): return ComponentAccess<JointComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::LINE_RENDERER): return ComponentAccess<LineRendererComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::SPRITE_RENDERER): return ComponentAccess<SpriteRendererComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::SORTING_GROUP): return ComponentAccess<SortingGroupComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::TRAIL): return ComponentAccess<TrailComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::MESH_TRAIL): return ComponentAccess<MeshTrailComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::MESH_RENDERER): return ComponentAccess<MeshRenderer>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::SKINNED_MESH_RENDERER): return ComponentAccess<SkinnedMeshRenderer>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::MOTION_WARP): return ComponentAccess<MotionWarpComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::IK_SOLVER): return ComponentAccess<IKSolverComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::RAGDOLL): return ComponentAccess<RagdollComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::SPRING_BONE): return ComponentAccess<SpringBoneComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::UI_ANIMATOR): return ComponentAccess<UIAnimator>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::NAV_MESH_PATROL): return ComponentAccess<NavMeshPatrolComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::SPLINE_FOLLOWER): return ComponentAccess<SplineFollowerComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::CAMERA_SHAKE): return ComponentAccess<CameraShakeComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::VIRTUAL_CAMERA): return ComponentAccess<VirtualCameraComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::AUDIO_MIXER_SEND): return ComponentAccess<AudioMixerSendComponent>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::FLOW_FIELD): return ComponentAccess<FlowField>(target, op, exists, lifetime);
    case static_cast<uint32_t>(ManagedNativeComponentKind::INSTANCED_MESH): return ComponentAccess<InstancedMeshComponent>(target, op, exists, lifetime);
    default: return ManagedBridgeStatus::UNSUPPORTED;
    }
}

bool ManagedObjectControls::IsPendingComponentRemoval(const GameObject& target, uint32_t kind)
{
    return kind < target.m_pendingManagedComponentRemovals.size() && target.m_pendingManagedComponentRemovals.test(kind);
}

bool ManagedObjectControls::SetParent(GameObject& target, GameObject* parent)
{
    Scene* scene = target.GetScene();
    if (!scene || (parent && parent->GetScene() != scene)) return false;
    const size_t slots = scene->EntitySlotCount();
    std::vector<bool> ancestors(slots);
    for (GameObject* current = parent; current; current = current->GetParent()) {
        const uint32_t index = current->GetID().index;
        if (current == &target || index >= slots || ancestors[index] || scene->GetGameObject(current->GetID()) != current) return false;
        ancestors[index] = true;
    }
    const auto finite = [](const math::Vector3& value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); };
    const auto rotation = [](const math::Quaternion& value) {
        const float length = value.Length();
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w)
            && std::isfinite(length) && length >= 1e-6f;
    };
    struct Prepared { GameObject* object; Transform transform; size_t parentIndex; };
    std::vector<Prepared> prepared{{&target, target.transform, EntityID::INVALID_INDEX}};
    std::vector<bool> seen(slots);
    for (size_t index = 0; index < prepared.size(); ++index) {
        GameObject* object = prepared[index].object;
        if (!object || object->GetID().index >= slots || object->GetScene() != scene || seen[object->GetID().index]
            || scene->GetGameObject(object->GetID()) != object) return false;
        seen[object->GetID().index] = true;
        Transform& transform = prepared[index].transform;
        const Transform* parentTransform = index == 0 ? (parent ? &parent->transform : nullptr) : &prepared[prepared[index].parentIndex].transform;
        if (!finite(transform.position) || !finite(transform.scale) || !rotation(transform.rotation)
            || (parentTransform && !rotation(parentTransform->worldRotation * transform.rotation))) return false;
        UpdateWorldTransform(transform, parentTransform);
        if (!finite(transform.worldPosition) || !finite(transform.worldScale) || !rotation(transform.worldRotation)) return false;
        for (int childIndex = 0; childIndex < object->GetChildCount(); ++childIndex) {
            GameObject* child = object->GetChild(childIndex);
            if (!child || child->GetParent() != object || prepared.size() >= slots) return false;
            prepared.push_back({child, child->transform, index});
        }
    }
    if (!target.SetParent(parent)) return false;
    for (const Prepared& item : prepared) item.object->transform = item.transform;
    return true;
}

void ManagedObjectControls::FlushScriptRemovals(Scene& scene)
{
    std::vector<EntityID> objects;
    for (GameObject& object : scene.GameObjects()) objects.push_back(object.GetID());
    for (EntityID id : objects) {
        GameObject* object = scene.GetGameObject(id);
        if (!object) continue;
        const auto removed = object->m_pendingManagedComponentRemovals;
        object->m_pendingManagedComponentRemovals.reset();
        const auto remove = [&]<typename T>(uint32_t kind) {
            if (kind >= removed.size() || !removed.test(kind)) return;
            if constexpr (std::is_base_of_v<ColliderComponent, T>) {
                if (auto* collider = object->GetComponent<T>())
                    for (RigidBodyComponent* body : scene.GetComponents<RigidBodyComponent>())
                        if (body->rigidBody && body->rigidBody->GetInertiaCollider() == collider->collider.get()) body->rigidBody->SetInertiaFromCollider(nullptr);
            }
            object->RemoveComponent<T>();
        };
        remove.template operator()<RigidBodyComponent>(1);
        remove.template operator()<BoxColliderComponent>(2);
        remove.template operator()<SphereColliderComponent>(3);
        remove.template operator()<CapsuleColliderComponent>(4);
        remove.template operator()<AnimatorComponent>(5);
        remove.template operator()<AudioSourceComponent>(6);
        remove.template operator()<CameraComponent>(8);
        remove.template operator()<NavMeshAgentComponent>(9);
        remove.template operator()<CharacterControllerComponent>(10);
        remove.template operator()<UIText>(11);
        remove.template operator()<UIImage>(12);
        remove.template operator()<UIButton>(13);
        remove.template operator()<UICanvas>(14);
        remove.template operator()<CylinderColliderComponent>(15);
        remove.template operator()<AabbColliderComponent>(16);
        remove.template operator()<MeshColliderComponent>(17);
        remove.template operator()<ConvexHullColliderComponent>(18);
        remove.template operator()<TerrainColliderComponent>(19);
        remove.template operator()<UISlider>(20);
        remove.template operator()<UIToggle>(21);
        remove.template operator()<UIScrollView>(22);
        remove.template operator()<UIInputField>(23);
        remove.template operator()<UICanvasGroup>(24);
        remove.template operator()<UIDragSource>(25);
        remove.template operator()<UIDropTarget>(26);
        remove.template operator()<LightComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::LIGHT));
        remove.template operator()<ParticleEmitter>(static_cast<uint32_t>(ManagedNativeComponentKind::PARTICLE_SYSTEM));
        remove.template operator()<MaterialComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::MATERIAL));
        remove.template operator()<VFXComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::VFX));
        remove.template operator()<SequencePlayerComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::SEQUENCE));
        remove.template operator()<TerrainComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::TERRAIN));
        remove.template operator()<WaterComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::WATER));
        remove.template operator()<JointComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::JOINT));
        remove.template operator()<LineRendererComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::LINE_RENDERER));
        remove.template operator()<SpriteRendererComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::SPRITE_RENDERER));
        remove.template operator()<SortingGroupComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::SORTING_GROUP));
        remove.template operator()<TrailComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::TRAIL));
        remove.template operator()<MeshTrailComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::MESH_TRAIL));
        remove.template operator()<MeshRenderer>(static_cast<uint32_t>(ManagedNativeComponentKind::MESH_RENDERER));
        remove.template operator()<SkinnedMeshRenderer>(static_cast<uint32_t>(ManagedNativeComponentKind::SKINNED_MESH_RENDERER));
        remove.template operator()<MotionWarpComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::MOTION_WARP));
        remove.template operator()<IKSolverComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::IK_SOLVER));
        remove.template operator()<RagdollComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::RAGDOLL));
        remove.template operator()<SpringBoneComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::SPRING_BONE));
        remove.template operator()<UIAnimator>(static_cast<uint32_t>(ManagedNativeComponentKind::UI_ANIMATOR));
        remove.template operator()<NavMeshPatrolComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::NAV_MESH_PATROL));
        remove.template operator()<SplineFollowerComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::SPLINE_FOLLOWER));
        remove.template operator()<CameraShakeComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::CAMERA_SHAKE));
        remove.template operator()<VirtualCameraComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::VIRTUAL_CAMERA));
        remove.template operator()<AudioMixerSendComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::AUDIO_MIXER_SEND));
        remove.template operator()<FlowField>(static_cast<uint32_t>(ManagedNativeComponentKind::FLOW_FIELD));
        remove.template operator()<InstancedMeshComponent>(static_cast<uint32_t>(ManagedNativeComponentKind::INSTANCED_MESH));
    }
    const auto entities = scene.GetEntities<ScriptComponent>();
    const std::vector<EntityID> snapshot(entities.begin(), entities.end());
    for (EntityID id : snapshot) {
        auto* component = scene.GetComponent<ScriptComponent>(id);
        auto* object = scene.GetGameObject(id);
        if (!component || !object) continue;
        for (size_t index = component->scripts.size(); index > 0; --index) {
            ScriptEntry& entry = component->scripts[index - 1];
            if (!entry.m_pendingRemoval) continue;
            if (entry.script) {
                entry.script->SetContext(&scene, object);
                entry.script->ExecuteProfiledCallback(&Script::OnDestroy, ScriptCallbackKind::DESTROY, "OnDestroy");
            }
            component->scripts.erase(component->scripts.begin() + static_cast<ptrdiff_t>(index - 1));
        }
    }
}

ManagedBridgeStatus ManagedObjectControls::Query(Scene& scene, GameObject* root, uint32_t scope, std::string_view name,
    std::string_view tag, std::string_view part, bool includeInactive, std::vector<GameObject*>& results)
{
    if (scope > 3 || (scope != 0 && !root)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (root && root->GetScene() != &scene) return ManagedBridgeStatus::SCENE_MISMATCH;
    struct Node { GameObject* object; bool active; uint32_t depth; };
    std::vector<Node> stack;
    std::vector<GameObject*> roots;
    if (scope == 0) {
        for (GameObject& object : scene.GameObjects()) if (!object.GetParent()) roots.push_back(&object);
    } else roots.push_back(root);
    for (auto item = roots.rbegin(); item != roots.rend(); ++item) stack.push_back({*item, true, 0});
    const size_t slots = scene.EntitySlotCount();
    std::vector<bool> seen(slots);
    std::vector<GameObject*> prepared;
    size_t visited = 0;
    while (!stack.empty()) {
        Node node = stack.back(); stack.pop_back();
        GameObject* object = node.object;
        if (!object || object->GetScene() != &scene || object->GetID().index >= slots
            || scene.GetGameObject(object->GetID()) != object || seen[object->GetID().index])
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        seen[object->GetID().index] = true; ++visited;
        const bool active = node.active && object->activeSelf();
        const bool inScope = scope == 0 || (scope == 1 && node.depth == 1) || (scope == 2 && node.depth != 0) || scope == 3;
        if (inScope && (includeInactive || active) && (name.empty() || object->name == name)
            && (tag.empty() || object->tag == tag) && (part.empty() || object->prefabSourceId == part)) prepared.push_back(object);
        for (int index = object->GetChildCount() - 1; index >= 0; --index) {
            GameObject* child = object->GetChild(index);
            if (!child || child->GetParent() != object) return ManagedBridgeStatus::INVALID_ARGUMENT;
            stack.push_back({child, active, node.depth + 1});
        }
    }
    if (scope == 0 && visited != scene.GameObjectCount()) return ManagedBridgeStatus::INVALID_ARGUMENT;
    /// @note 指定 root より上の有効状態も条件に含め、破損した親経路は有限回で拒否する。
    if (scope != 0 && !includeInactive) {
        GameObject* parent = root->GetParent();
        std::vector<bool> ancestors(slots);
        while (parent) {
            const uint32_t index = parent->GetID().index;
            if (index >= slots || ancestors[index] || seen[index] || scene.GetGameObject(parent->GetID()) != parent)
                return ManagedBridgeStatus::INVALID_ARGUMENT;
            ancestors[index] = true;
            if (!parent->activeSelf()) prepared.clear();
            parent = parent->GetParent();
        }
    }
    results = std::move(prepared);
    return ManagedBridgeStatus::OK;
}
} /// @note namespace fbzz::scene::managed_detail
