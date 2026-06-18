// FBZZ Engine
// PhysicsSystem.cpp | fbzz::scene
// Scene と physics::World の同期
// RigidBodyComponent と ColliderComponent を physics に反映し、Step 後に Transform へ戻す。
// Scene から physics への依存方向を保つ。
#include "Engine/Scene/Systems/PhysicsSystem.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/CharacterControllerComponent.hpp"
#include "Engine/Scene/Components/ColliderComponent.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/RigidBodyComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Scene/Components/VolumeComponent.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include "Engine/Scene/Components/WaterComponent.hpp"
#include "Engine/Scene/ScriptComponent.hpp"
#include <Engine/Core/Time.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Physics/ColliderVolume.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/HeightFieldCollider.hpp>
#include <Physics/TriangleMeshCollider.hpp>
#include <Physics/World.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cmath>
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

// WaterBuoyancyVolume — WaterComponent の Gerstner 波を CPU 側で評価する浮力 Volume。
// WHY: physics モジュールに WaterComponent 依存を入れると依存方向が逆転するため、
//      Engine の PhysicsSystem 内で physics::Volume を実装し、World には抽象 Volume として渡す。
class WaterBuoyancyVolume final : public physics::Volume {
public:
    WaterBuoyancyVolume(const WaterComponent& water,
                        const Transform& transform,
                        const physics::VolumeSettings& settings)
        : m_water(water)
        , m_position(transform.worldPosition)
        , m_settings(settings)
        , m_time(Time::time)
    {
    }

    bool Contains(const math::Vector3& position) const override
    {
        const float localX = position.x - m_position.x;
        const float localZ = position.z - m_position.z;
        if (std::abs(localX) > m_water.extentX * 0.5f || std::abs(localZ) > m_water.extentZ * 0.5f)
            return false;
        const float surfaceY = SurfaceY(localX, localZ);
        const float bottomY  = surfaceY - 10.0f;
        return position.y <= surfaceY && position.y >= bottomY;
    }

    void Apply(physics::RigidBody& body, float /*dt*/) override
    {
        if (body.IsStatic()) return;

        const math::Vector3 pos = body.GetPosition();
        const float localX = pos.x - m_position.x;
        const float localZ = pos.z - m_position.z;
        const float surfaceY = SurfaceY(localX, localZ);
        const float depth = (std::max)(surfaceY - pos.y, 0.0f);
        const float submersion = math::Clamp01(depth / 10.0f);

        // WHY: Water の浮力・抵抗は毎 substep 適用される環境力。
        //      ApplyForce() で WakeUp すると、水面範囲内の静止 body が永久に Sleep できず World::Step が重くなる。
        body.ApplyForceNoWake(math::Vector3::UP * (m_settings.buoyancy * body.GetMass() * submersion));
        body.ApplyForceNoWake(-body.GetVelocity() * (m_settings.drag * submersion));
    }

    void Tick(float dt) override
    {
        m_time += dt;
    }

private:
    float SurfaceY(float localX, float localZ) const
    {
        return m_position.y + m_water.GetSurfaceHeightAt(localX, localZ, m_time);
    }

    WaterComponent m_water;
    math::Vector3 m_position;
    physics::VolumeSettings m_settings;
    float m_time = 0.0f;
};

math::Vector3 ComponentScale(const math::Vector3& a, const math::Vector3& b)
{
    return { a.x * b.x, a.y * b.y, a.z * b.z };
}

math::Vector3 ColliderWorldCenter(const GameObject& go, const ColliderComponent& col)
{
    return go.transform.worldPosition + go.transform.worldRotation * ComponentScale(col.center, go.transform.worldScale);
}

void WriteWorldPoseToTransform(GameObject& go,
                               const math::Vector3& worldPosition,
                               const math::Quaternion& worldRotation)
{
    auto& tf = go.transform;
    if (auto* parent = go.GetParent()) {
        const auto& parentTf = parent->transform;
        const math::Quaternion invParentRot = parentTf.worldRotation.Inverse();
        const math::Vector3 parentSpace = invParentRot * (worldPosition - parentTf.worldPosition);

        // WHY: TransformSystem は localPosition に親 worldScale を掛けてから親回転を適用する。
        //      Physics は world pose を返すため、ここで同じ式を逆変換して local pose に戻す。
        tf.position = {
            parentTf.worldScale.x == 0.0f ? 0.0f : parentSpace.x / parentTf.worldScale.x,
            parentTf.worldScale.y == 0.0f ? 0.0f : parentSpace.y / parentTf.worldScale.y,
            parentTf.worldScale.z == 0.0f ? 0.0f : parentSpace.z / parentTf.worldScale.z
        };
        tf.rotation = (invParentRot * worldRotation).Normalized();
    } else {
        tf.position = worldPosition;
        tf.rotation = worldRotation;
    }

    tf.worldPosition = worldPosition;
    tf.worldRotation = worldRotation;
}

void SyncColliderShape(ColliderComponent&) {}

void SyncColliderShape(AabbColliderComponent& col)
{
    auto* shape = col.collider && col.collider->GetType() == physics::ColliderType::AABB
        ? static_cast<physics::AABBCollider*>(col.collider.get())
        : nullptr;
    if (!shape) {
        col.collider = std::make_unique<physics::AABBCollider>(col.size * 0.5f);
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
        col.collider = std::make_unique<physics::OBBCollider>(col.size * 0.5f);
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
        col.collider = std::make_unique<physics::SphereCollider>(col.radius);
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
        col.collider = std::make_unique<physics::CapsuleCollider>(col.radius, col.halfHeight);
        shape = static_cast<physics::CapsuleCollider*>(col.collider.get());
    }
    shape->m_radius = col.radius;
    shape->m_halfHeight = col.halfHeight;
}

void EnsureMeshCollider(GameObject& go, MeshColliderComponent& col)
{
    if (col.collider) return;
    renderer::Mesh* mesh = nullptr;
    if (auto* meshRenderer = go.GetComponent<MeshRenderer>())
        mesh = meshRenderer->mesh;
    if (!mesh) {
        if (auto* skinned = go.GetComponent<SkinnedMeshRenderer>()) {
            if (!skinned->model && !skinned->modelPath.empty())
                skinned->model = asset::AssetManager::LoadModel(skinned->modelPath);
            if (skinned->model && skinned->meshIndex >= 0 &&
                skinned->meshIndex < static_cast<int>(skinned->model->meshes.size()))
                mesh = skinned->model->meshes[static_cast<size_t>(skinned->meshIndex)].get();
        }
    }
    if (!mesh && !col.meshPath.empty()) {
        if (auto* model = asset::AssetManager::LoadModel(col.meshPath)) {
            if (col.meshIndex >= 0 && col.meshIndex < static_cast<int>(model->meshes.size()))
                mesh = model->meshes[static_cast<size_t>(col.meshIndex)].get();
        }
    }
    if (!mesh || mesh->cpuVertices.empty() || mesh->cpuIndices.empty()) return;

    std::vector<math::Vector3> positions;
    positions.reserve(mesh->cpuVertices.size());
    for (const auto& vertex : mesh->cpuVertices)
        positions.push_back(vertex.position);
    col.collider = std::make_unique<physics::TriangleMeshCollider>(positions, mesh->cpuIndices);
}

void EnsureConvexHullCollider(GameObject& go, ConvexHullColliderComponent& col)
{
    if (col.collider) return;
    renderer::Mesh* mesh = nullptr;
    if (auto* meshRenderer = go.GetComponent<MeshRenderer>())
        mesh = meshRenderer->mesh;
    if (!mesh) {
        if (auto* skinned = go.GetComponent<SkinnedMeshRenderer>()) {
            if (!skinned->model && !skinned->modelPath.empty())
                skinned->model = asset::AssetManager::LoadModel(skinned->modelPath);
            if (skinned->model && skinned->meshIndex >= 0 &&
                skinned->meshIndex < static_cast<int>(skinned->model->meshes.size()))
                mesh = skinned->model->meshes[static_cast<size_t>(skinned->meshIndex)].get();
        }
    }
    if (!mesh && !col.meshPath.empty()) {
        if (auto* model = asset::AssetManager::LoadModel(col.meshPath)) {
            if (col.meshIndex >= 0 && col.meshIndex < static_cast<int>(model->meshes.size()))
                mesh = model->meshes[static_cast<size_t>(col.meshIndex)].get();
        }
    }
    if (!mesh || mesh->cpuVertices.empty()) return;

    std::vector<math::Vector3> positions;
    positions.reserve(mesh->cpuVertices.size());
    for (const auto& vertex : mesh->cpuVertices)
        positions.push_back(vertex.position);
    col.collider = std::make_unique<physics::ConvexHullCollider>(std::move(positions));
}

template<typename T>
void AddColliderInstance(Scene& scene,
                         GameObject& go,
                         T& col,
                         physics::World& world,
                         ColliderOwnerMap& colliderOwners,
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
        mesh->UpdateWithScale(worldCenter, go.transform.worldRotation, scale);
    } else if (auto* hf = col.collider->GetType() == physics::ColliderType::HEIGHT_FIELD
            ? static_cast<physics::HeightFieldCollider*>(col.collider.get())
            : nullptr) {
        hf->UpdateWithScale(worldCenter, go.transform.worldRotation, go.transform.worldScale);
    } else if (auto* hull = col.collider->GetType() == physics::ColliderType::CONVEX_HULL
            ? static_cast<physics::ConvexHullCollider*>(col.collider.get())
            : nullptr) {
        math::Vector3 scale = go.transform.worldScale;
        if constexpr (std::is_same_v<T, ConvexHullColliderComponent>) {
            if (!col.useTransformScale)
                scale = math::Vector3::ONE;
        }
        hull->UpdateWithScale(worldCenter, go.transform.worldRotation, scale);
    } else {
        col.collider->Update(worldCenter, go.transform.worldRotation);
    }
    const math::Vector3 centerOffset = ComponentScale(col.center, go.transform.worldScale);
    physics::ColliderInstance instance{ col.collider.get(), body, &col.material, centerOffset, col.isTrigger, go.layer };
    col.colliderHandle = world.SyncCollider(col.colliderHandle, instance);
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
        if (settings.type == physics::VolumeType::Buoyancy) {
            if (auto* water = go.GetComponent<WaterComponent>()) {
                volume->volumeHandle = world.SyncVolume(
                    volume->volumeHandle,
                    std::make_unique<WaterBuoyancyVolume>(*water, go.transform, settings));
                return;
            }
        }

        volume->volumeHandle = world.SyncVolume(
            volume->volumeHandle,
            std::make_unique<physics::ColliderVolume>(col.collider.get(), settings));
    }
}

void SyncTerrainCollider(Scene& scene, GameObject& go, TerrainColliderComponent& col)
{
    auto* terrain = go.GetComponent<TerrainComponent>();
    if (!terrain || !terrain->enabled || terrain->heightData.empty()) return;

    if (terrain->colliderDirty || !col.collider) {
        if (auto* hf = col.collider
                && col.collider->GetType() == physics::ColliderType::HEIGHT_FIELD
                ? static_cast<physics::HeightFieldCollider*>(col.collider.get())
                : nullptr) {
            // 既存 HeightFieldCollider に heightData だけ再適用し BVH を再構築する。
            // WHY: オブジェクト生成コストを省き、WorldHandle を維持したまま再構築できる。
            hf->Rebuild(terrain->heightData,
                        terrain->rows, terrain->columns,
                        terrain->cellSize, terrain->maxHeight);
        } else {
            col.collider = std::make_unique<physics::HeightFieldCollider>(
                terrain->heightData,
                terrain->rows, terrain->columns,
                terrain->cellSize, terrain->maxHeight);
        }
        terrain->colliderDirty = false;
    }
}

template<typename T>
void SyncColliderComponents(Scene& scene,
                            physics::World& world,
                            ColliderOwnerMap& colliderOwners,
                            float dt)
{
    // WHY: GameObject 全体を毎 fixed step 走査して各 Collider 型を GetComponent すると、
    //      物理を持たないオブジェクト数に比例して PhysicsSystem 側の固定コストが増える。
    // WHAT: ComponentArray が保持する Entity span を入口にし、存在する Collider component だけを同期する。
    for (EntityID id : scene.GetEntities<T>()) {
        GameObject* go = scene.GetGameObject(id);
        T* col = scene.GetComponent<T>(id);
        if (!go || !col) continue;

        if constexpr (std::is_same_v<T, MeshColliderComponent>) {
            EnsureMeshCollider(*go, *col);
        } else if constexpr (std::is_same_v<T, ConvexHullColliderComponent>) {
            EnsureConvexHullCollider(*go, *col);
        } else if constexpr (std::is_same_v<T, TerrainColliderComponent>) {
            SyncTerrainCollider(scene, *go, *col);
        }

        AddColliderInstance(scene, *go, *col, world, colliderOwners, dt);
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
    if (!scriptComponent) return;

    CollisionInfo info;
    info.self = self.gameObject;
    info.other = other.gameObject;
    info.selfCollider = self.collider;
    info.otherCollider = other.collider;
    info.contactNormal = contactNormal;
    info.contactPoint = contactPoint;
    info.contactDepth = contactDepth;

    for (auto& entry : scriptComponent->scripts) {
        if (!entry.script || !entry.script->enabled) continue;
        entry.script->SetContext(&scene, self.gameObject);
        (entry.script.get()->*callback)(info);
    }
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

ComponentAccess PhysicsSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<ColliderComponent, RigidBodyComponent, CharacterControllerComponent>()
        .Writes<RigidBodyComponent, VolumeComponent>();
}

void PhysicsSystem::Update(SystemContext& ctx) {
    Scene& scene = ctx.scene;
    physics::World& world = ctx.world;
    const float dt = ctx.fixedDt;
    FBZZ_PROFILE_SCOPE("PhysicsSystem");

    Script::SetPhysicsWorld(&world);

    static thread_local ColliderOwnerMap colliderOwners;
    colliderOwners.clear();
    colliderOwners.reserve(
        scene.GetEntities<AabbColliderComponent>().size() +
        scene.GetEntities<BoxColliderComponent>().size() +
        scene.GetEntities<SphereColliderComponent>().size() +
        scene.GetEntities<CapsuleColliderComponent>().size() +
        scene.GetEntities<MeshColliderComponent>().size() +
        scene.GetEntities<ConvexHullColliderComponent>().size() +
        scene.GetEntities<TerrainColliderComponent>().size());

    world.BeginSceneSync();

    {
        FBZZ_PROFILE_SCOPE("PhysicsSystem::SyncRigidBodies");
        for (EntityID id : scene.GetEntities<RigidBodyComponent>()) {
            GameObject* go = scene.GetGameObject(id);
            auto* rb = scene.GetComponent<RigidBodyComponent>(id);
            if (!go || !rb || !rb->enabled || !rb->rigidBody) continue;

            // WHY: BodyHandle は Component 側へ永続化される runtime state。
            //      SceneView の structured binding に依存せず、Component 実体へ直接書き戻す。
            rb->rigidBody->SetPosition(go->transform.worldPosition);
            rb->rigidBody->SetRotation(go->transform.worldRotation);
            rb->bodyHandle = world.SyncBody(rb->bodyHandle, rb->rigidBody.get());
        }
    }

    {
        FBZZ_PROFILE_SCOPE("PhysicsSystem::SyncColliders");
        SyncColliderComponents<AabbColliderComponent>(scene, world, colliderOwners, dt);
        SyncColliderComponents<BoxColliderComponent>(scene, world, colliderOwners, dt);
        SyncColliderComponents<SphereColliderComponent>(scene, world, colliderOwners, dt);
        SyncColliderComponents<CapsuleColliderComponent>(scene, world, colliderOwners, dt);
        SyncColliderComponents<MeshColliderComponent>(scene, world, colliderOwners, dt);
        SyncColliderComponents<ConvexHullColliderComponent>(scene, world, colliderOwners, dt);
        SyncColliderComponents<TerrainColliderComponent>(scene, world, colliderOwners, dt);
    }

    {
        FBZZ_PROFILE_SCOPE("PhysicsSystem::SubmitSceneState");
        world.EndSceneSync();
    }
    {
        FBZZ_PROFILE_SCOPE("physics::World::Step");
        world.Step(dt);
    }
    {
        FBZZ_PROFILE_SCOPE("PhysicsSystem::DispatchCollisionEvents");
        DispatchCollisionEvents(scene, world, colliderOwners);
    }

    {
        FBZZ_PROFILE_SCOPE("PhysicsSystem::WriteBackTransforms");
        for (EntityID id : scene.GetEntities<RigidBodyComponent>()) {
            GameObject* go = scene.GetGameObject(id);
            auto* rb = scene.GetComponent<RigidBodyComponent>(id);
            if (!go || !rb || !rb->enabled || !rb->rigidBody) continue;
            WriteWorldPoseToTransform(*go, rb->rigidBody->GetPosition(), rb->rigidBody->GetRotation());
        }
    }
}

} // namespace fbzz::scene
