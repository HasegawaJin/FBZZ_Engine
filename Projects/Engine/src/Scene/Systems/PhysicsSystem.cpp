// FBZZ Engine
// PhysicsSystem.cpp | fbzz::scene
// Scene と physics::World の同期
// RigidBodyComponent と ColliderComponent を physics に反映し、Step 後に Transform へ戻す。
// Scene から physics への依存方向を保つ。
#include "Engine/Scene/Systems/PhysicsSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/ColliderComponent.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/RigidBodyComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Scene/Components/VolumeComponent.hpp"
#include "Engine/Scene/ScriptComponent.hpp"
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Physics/ColliderVolume.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <Physics/World.hpp>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {

namespace {

struct ColliderOwner {
    GameObject* gameObject = nullptr;
    ColliderComponent* collider = nullptr;
};

// physics::CollisionEvent は Collider* のペアを持つが Scene 側の情報を持たない。
// このマップで Collider* → (GameObject*, ColliderComponent*) を O(1) で逆引きし、
// Script へのコールバック発火で使う。
using ColliderOwnerMap = std::unordered_map<const physics::Collider*, ColliderOwner>;
using ScriptCollisionCallback = void (Script::*)(const CollisionInfo&);

math::Vector3 ComponentScale(const math::Vector3& a, const math::Vector3& b)
{
    return { a.x * b.x, a.y * b.y, a.z * b.z };
}

math::Vector3 ColliderWorldCenter(const GameObject& go, const ColliderComponent& col)
{
    return go.transform.position + go.transform.rotation * ComponentScale(col.center, go.transform.worldScale);
}

void SyncColliderShape(ColliderComponent&) {}

void SyncColliderShape(AabbColliderComponent& col)
{
    auto* shape = col.collider && col.collider->GetType() == physics::ColliderType::AABB
        ? static_cast<physics::AABBCollider*>(col.collider.get())
        : nullptr;
    if (!shape) {
        col.collider = std::make_shared<physics::AABBCollider>(col.size * 0.5f);
        shape = static_cast<physics::AABBCollider*>(col.collider.get());
    }
    shape->m_halfExtents = col.size * 0.5f;
}

void SyncColliderShape(BoxColliderComponent& col)
{
    auto* shape = col.collider && col.collider->GetType() == physics::ColliderType::OBB
        ? static_cast<physics::OBBCollider*>(col.collider.get())
        : nullptr;
    if (!shape) {
        col.collider = std::make_shared<physics::OBBCollider>(col.size * 0.5f);
        shape = static_cast<physics::OBBCollider*>(col.collider.get());
    }
    shape->m_halfExtents = col.size * 0.5f;
}

void SyncColliderShape(SphereColliderComponent& col)
{
    auto* shape = col.collider && col.collider->GetType() == physics::ColliderType::SPHERE
        ? static_cast<physics::SphereCollider*>(col.collider.get())
        : nullptr;
    if (!shape) {
        col.collider = std::make_shared<physics::SphereCollider>(col.radius);
        shape = static_cast<physics::SphereCollider*>(col.collider.get());
    }
    shape->m_radius = col.radius;
}

void SyncColliderShape(CapsuleColliderComponent& col)
{
    auto* shape = col.collider && col.collider->GetType() == physics::ColliderType::CAPSULE
        ? static_cast<physics::CapsuleCollider*>(col.collider.get())
        : nullptr;
    if (!shape) {
        col.collider = std::make_shared<physics::CapsuleCollider>(col.radius, col.halfHeight);
        shape = static_cast<physics::CapsuleCollider*>(col.collider.get());
    }
    shape->m_radius = col.radius;
    shape->m_halfHeight = col.halfHeight;
}

void EnsureMeshCollider(GameObject& go, MeshColliderComponent& col)
{
    if (col.collider) return;
    std::shared_ptr<renderer::Mesh> mesh;
    if (auto* meshRenderer = go.GetComponent<MeshRenderer>())
        mesh = meshRenderer->mesh;
    if (!mesh) {
        if (auto* skinned = go.GetComponent<SkinnedMeshRenderer>()) {
            if (!skinned->model && !skinned->modelPath.empty())
                skinned->model = asset::AssetManager::Load<asset::Model>(skinned->modelPath);
            if (skinned->model && skinned->meshIndex >= 0 &&
                skinned->meshIndex < static_cast<int>(skinned->model->meshes.size()))
                mesh = skinned->model->meshes[static_cast<size_t>(skinned->meshIndex)];
        }
    }
    if (!mesh && !col.meshPath.empty()) {
        if (auto model = asset::AssetManager::Load<asset::Model>(col.meshPath)) {
            if (col.meshIndex >= 0 && col.meshIndex < static_cast<int>(model->meshes.size()))
                mesh = model->meshes[static_cast<size_t>(col.meshIndex)];
        }
    }
    if (!mesh || mesh->cpuVertices.empty() || mesh->cpuIndices.empty()) return;

    std::vector<math::Vector3> positions;
    positions.reserve(mesh->cpuVertices.size());
    for (const auto& vertex : mesh->cpuVertices)
        positions.push_back(vertex.position);
    col.collider = std::make_shared<physics::TriangleMeshCollider>(positions, mesh->cpuIndices);
}

void EnsureConvexHullCollider(GameObject& go, ConvexHullColliderComponent& col)
{
    if (col.collider) return;
    std::shared_ptr<renderer::Mesh> mesh;
    if (auto* meshRenderer = go.GetComponent<MeshRenderer>())
        mesh = meshRenderer->mesh;
    if (!mesh) {
        if (auto* skinned = go.GetComponent<SkinnedMeshRenderer>()) {
            if (!skinned->model && !skinned->modelPath.empty())
                skinned->model = asset::AssetManager::Load<asset::Model>(skinned->modelPath);
            if (skinned->model && skinned->meshIndex >= 0 &&
                skinned->meshIndex < static_cast<int>(skinned->model->meshes.size()))
                mesh = skinned->model->meshes[static_cast<size_t>(skinned->meshIndex)];
        }
    }
    if (!mesh && !col.meshPath.empty()) {
        if (auto model = asset::AssetManager::Load<asset::Model>(col.meshPath)) {
            if (col.meshIndex >= 0 && col.meshIndex < static_cast<int>(model->meshes.size()))
                mesh = model->meshes[static_cast<size_t>(col.meshIndex)];
        }
    }
    if (!mesh || mesh->cpuVertices.empty()) return;

    std::vector<math::Vector3> positions;
    positions.reserve(mesh->cpuVertices.size());
    for (const auto& vertex : mesh->cpuVertices)
        positions.push_back(vertex.position);
    col.collider = std::make_shared<physics::ConvexHullCollider>(std::move(positions));
}

template<typename T>
void AddColliderInstance(GameObject& go,
                         T& col,
                         std::vector<physics::ColliderInstance>& colliders,
                         ColliderOwnerMap& colliderOwners,
                         std::vector<std::shared_ptr<physics::Volume>>& volumes,
                         float dt)
{
    if (!col.enabled || !col.collider) return;

    auto* rb = go.GetComponent<RigidBodyComponent>();
    physics::RigidBody* body = rb && rb->enabled && rb->rigidBody ? rb->rigidBody.get() : nullptr;

    SyncColliderShape(col);
    const math::Vector3 worldCenter = ColliderWorldCenter(go, col);
    if (auto* mesh = col.collider->GetType() == physics::ColliderType::TRIANGLE_MESH
            ? static_cast<physics::TriangleMeshCollider*>(col.collider.get())
            : nullptr) {
        math::Vector3 scale = go.transform.worldScale;
        if constexpr (std::is_same_v<T, MeshColliderComponent> ||
                      std::is_same_v<T, ConvexHullColliderComponent>) {
            if (!col.useTransformScale)
                scale = math::Vector3::ONE;
        }
        mesh->UpdateWithScale(worldCenter, go.transform.rotation, scale);
    } else if (auto* hull = col.collider->GetType() == physics::ColliderType::CONVEX_HULL
            ? static_cast<physics::ConvexHullCollider*>(col.collider.get())
            : nullptr) {
        math::Vector3 scale = go.transform.worldScale;
        if constexpr (std::is_same_v<T, ConvexHullColliderComponent>) {
            if (!col.useTransformScale)
                scale = math::Vector3::ONE;
        }
        hull->UpdateWithScale(worldCenter, go.transform.rotation, scale);
    } else {
        col.collider->Update(worldCenter, go.transform.rotation);
    }
    if (body)
        body->SetInertiaFromCollider(col.collider.get());

    const math::Vector3 centerOffset = ComponentScale(col.center, go.transform.worldScale);
    colliders.push_back({ col.collider, body, &col.material, centerOffset, col.isTrigger, go.layer });
    colliderOwners[col.collider.get()] = { &go, &col };

    auto* volume = go.GetComponent<VolumeComponent>();
    if (volume && volume->enabled && col.isTrigger) {
        if (volume->duration >= 0.0f && volume->elapsed >= volume->duration) return;
        if (volume->duration >= 0.0f) volume->elapsed += dt;

        physics::VolumeSettings settings;
        settings.type = volume->type;
        settings.gravity = volume->gravity;
        settings.magneticField = volume->magneticField;
        settings.swirlStrength = volume->swirlStrength;
        settings.inwardStrength = volume->inwardStrength;
        settings.liftStrength = volume->liftStrength;
        settings.buoyancy = volume->buoyancy;
        settings.drag = volume->drag;
        settings.explosionImpulse = volume->explosionImpulse;
        settings.timeScale = volume->timeScale;
        settings.duration = volume->duration;
        volumes.push_back(std::make_shared<physics::ColliderVolume>(col.collider, settings));
    }
}

void DispatchToScript(Scene& scene,
                      const ColliderOwner& self,
                      const ColliderOwner& other,
                      const math::Vector3& contactNormal,
                      const math::Vector3& contactPoint,
                      float contactDepth,
                      ScriptCollisionCallback callback)
{
    if (!self.gameObject || !other.gameObject) return;

    auto* scriptComponent = self.gameObject->GetComponent<ScriptComponent>();
    if (!scriptComponent || !scriptComponent->script || !scriptComponent->script->enabled) return;

    scriptComponent->script->SetContext(&scene, self.gameObject);
    CollisionInfo info;
    info.self = self.gameObject;
    info.other = other.gameObject;
    info.selfCollider = self.collider;
    info.otherCollider = other.collider;
    info.contactNormal = contactNormal;
    info.contactPoint = contactPoint;
    info.contactDepth = contactDepth;
    (scriptComponent->script.get()->*callback)(info);
}

void DispatchCollisionEvent(Scene& scene,
                            const ColliderOwnerMap& owners,
                            const physics::CollisionEvent& event,
                            ScriptCollisionCallback callback)
{
    auto ownerA = owners.find(event.colliderA);
    auto ownerB = owners.find(event.colliderB);
    if (ownerA == owners.end() || ownerB == owners.end()) return;

    DispatchToScript(scene, ownerA->second, ownerB->second,
                     event.normal, event.point, event.depth, callback);
    DispatchToScript(scene, ownerB->second, ownerA->second,
                     -event.normal, event.point, event.depth, callback);
}

void DispatchCollisionEvents(Scene& scene,
                             physics::World& world,
                             const ColliderOwnerMap& owners)
{
    for (const auto& event : world.GetEnterEvents())
    {
        DispatchCollisionEvent(scene, owners, event, event.isTrigger
            ? &Script::OnTriggerEnter
            : &Script::OnCollisionEnter);
    }

    for (const auto& event : world.GetStayEvents())
    {
        DispatchCollisionEvent(scene, owners, event, event.isTrigger
            ? &Script::OnTriggerStay
            : &Script::OnCollisionStay);
    }

    for (const auto& event : world.GetExitEvents())
    {
        DispatchCollisionEvent(scene, owners, event, event.isTrigger
            ? &Script::OnTriggerExit
            : &Script::OnCollisionExit);
    }
}

} // namespace

void PhysicsSystem(Scene& scene, physics::World& world, float dt) {
    std::vector<std::shared_ptr<physics::RigidBody>> bodies;
    std::vector<physics::ColliderInstance> colliders;
    std::vector<std::shared_ptr<physics::Volume>> volumes;
    ColliderOwnerMap colliderOwners;

    for (auto [tf, rb] : scene.View<Transform, RigidBodyComponent>()) {
        if (!rb.enabled || !rb.rigidBody) continue;
        rb.rigidBody->SetPosition(tf.position);
        rb.rigidBody->SetRotation(tf.rotation);
        bodies.push_back(rb.rigidBody);
    }

    for (auto& go : scene.GameObjects()) {
        if (auto* col = go.GetComponent<AabbColliderComponent>())
            AddColliderInstance(go, *col, colliders, colliderOwners, volumes, dt);
        if (auto* col = go.GetComponent<BoxColliderComponent>())
            AddColliderInstance(go, *col, colliders, colliderOwners, volumes, dt);
        if (auto* col = go.GetComponent<SphereColliderComponent>())
            AddColliderInstance(go, *col, colliders, colliderOwners, volumes, dt);
        if (auto* col = go.GetComponent<CapsuleColliderComponent>())
            AddColliderInstance(go, *col, colliders, colliderOwners, volumes, dt);
        if (auto* col = go.GetComponent<MeshColliderComponent>()) {
            EnsureMeshCollider(go, *col);
            AddColliderInstance(go, *col, colliders, colliderOwners, volumes, dt);
        }
        if (auto* col = go.GetComponent<ConvexHullColliderComponent>()) {
            EnsureConvexHullCollider(go, *col);
            AddColliderInstance(go, *col, colliders, colliderOwners, volumes, dt);
        }
    }

    world.SetBodies(std::move(bodies));
    world.SetColliders(std::move(colliders));
    world.SetVolumes(std::move(volumes));
    world.Step(dt);
    DispatchCollisionEvents(scene, world, colliderOwners);

    for (auto [tf, rb] : scene.View<Transform, RigidBodyComponent>()) {
        if (!rb.enabled || !rb.rigidBody) continue;
        tf.localPosition = rb.rigidBody->GetPosition();
        tf.localRotation = rb.rigidBody->GetRotation();
        tf.position = tf.localPosition;
        tf.rotation = tf.localRotation;
    }
}

} // namespace fbzz::scene
