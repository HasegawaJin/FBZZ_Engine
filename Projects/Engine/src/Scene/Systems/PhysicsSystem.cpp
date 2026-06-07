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
        , m_position(transform.position)
        , m_settings(settings)
        , m_time(core::Time::TotalTime())
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
        const float depth = std::max(surfaceY - pos.y, 0.0f);
        const float submersion = math::Clamp01(depth / 10.0f);

        body.ApplyForce(math::Vector3::UP * (m_settings.buoyancy * body.GetMass() * submersion));
        body.ApplyForce(-body.GetVelocity() * (m_settings.drag * submersion));
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
void AddColliderInstance(Scene& scene,
                         GameObject& go,
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
        if (settings.type == physics::VolumeType::Buoyancy) {
            if (auto* water = go.GetComponent<WaterComponent>()) {
                volumes.push_back(std::make_shared<WaterBuoyancyVolume>(*water, go.transform, settings));
                return;
            }
        }

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

void PhysicsSystem(Scene& scene, physics::World& world, float dt) {
    FBZZ_PROFILE_SCOPE("PhysicsSystem");

    Script::SetPhysicsWorld(&world);

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

    // ── [Phase 7] TerrainComponent + MeshColliderComponent → TriangleMeshCollider 自動構築 ──
    // TerrainComponent と MeshColliderComponent の両方を持つ GO を検出し、
    // ハイトマップから三角形メッシュを自動的に構築する。
    // WHY: TriangleMeshCollider が既存の collision pipeline に乗れるため、
    //      新たな物理コードを追加せずに地形コリジョンを実現できる。
    //      colliderDirty フラグでエディタ彫刻後の再構築をトリガーする。
    for (auto& go : scene.GameObjects()) {
        auto* terrain = go.GetComponent<TerrainComponent>();
        auto* meshCol = go.GetComponent<MeshColliderComponent>();
        if (!terrain || !meshCol || !terrain->enabled || terrain->heightData.empty()) continue;

        // colliderDirty が立っているか未生成の場合のみ再構築する。
        // EnsureMeshCollider は collider が set 済みなら skip するため、
        // このブロックで先に set しておくことで自動ビルドが阻害されない。
        if (terrain->colliderDirty || !meshCol->collider) {
            const int cols = terrain->columns;
            const int rows = terrain->rows;

            // ローカル空間の頂点座標を生成する
            // WHY: TriangleMeshCollider の UpdateWithScale が Transform を適用するため、
            //      ここではローカル座標のみ渡す。
            std::vector<math::Vector3> positions;
            positions.reserve(static_cast<size_t>(cols) * static_cast<size_t>(rows));
            for (int z = 0; z < rows; ++z) {
                for (int x = 0; x < cols; ++x) {
                    const float h = terrain->heightData[
                        static_cast<size_t>(z) * static_cast<size_t>(cols) + static_cast<size_t>(x)]
                        * terrain->maxHeight;
                    positions.push_back({
                        static_cast<float>(x) * terrain->cellSize,
                        h,
                        static_cast<float>(z) * terrain->cellSize
                    });
                }
            }

            // クアッド → 2 三角形（頂点法線と揃えるため CW）
            std::vector<uint32_t> indices;
            indices.reserve(static_cast<size_t>(cols - 1) * static_cast<size_t>(rows - 1) * 6u);
            for (int z = 0; z < rows - 1; ++z) {
                for (int x = 0; x < cols - 1; ++x) {
                    const uint32_t i00 = static_cast<uint32_t>(z * cols + x);
                    const uint32_t i10 = i00 + 1u;
                    const uint32_t i01 = i00 + static_cast<uint32_t>(cols);
                    const uint32_t i11 = i01 + 1u;
                    indices.push_back(i00); indices.push_back(i01); indices.push_back(i10);
                    indices.push_back(i10); indices.push_back(i01); indices.push_back(i11);
                }
            }

            meshCol->collider = std::make_shared<physics::TriangleMeshCollider>(positions, indices);
            terrain->colliderDirty = false;
        }
    }

    for (auto& go : scene.GameObjects()) {
        if (auto* col = go.GetComponent<AabbColliderComponent>())
            AddColliderInstance(scene, go, *col, colliders, colliderOwners, volumes, dt);
        if (auto* col = go.GetComponent<BoxColliderComponent>())
            AddColliderInstance(scene, go, *col, colliders, colliderOwners, volumes, dt);
        if (auto* col = go.GetComponent<SphereColliderComponent>())
            AddColliderInstance(scene, go, *col, colliders, colliderOwners, volumes, dt);
        if (auto* col = go.GetComponent<CapsuleColliderComponent>())
            AddColliderInstance(scene, go, *col, colliders, colliderOwners, volumes, dt);
        if (auto* col = go.GetComponent<MeshColliderComponent>()) {
            EnsureMeshCollider(go, *col);
            AddColliderInstance(scene, go, *col, colliders, colliderOwners, volumes, dt);
        }
        if (auto* col = go.GetComponent<ConvexHullColliderComponent>()) {
            EnsureConvexHullCollider(go, *col);
            AddColliderInstance(scene, go, *col, colliders, colliderOwners, volumes, dt);
        }
    }

    {
        FBZZ_PROFILE_SCOPE("PhysicsSystem::SubmitSceneState");
        world.SetBodies(std::move(bodies));
        world.SetColliders(std::move(colliders));
        world.SetVolumes(std::move(volumes));
    }
    {
        FBZZ_PROFILE_SCOPE("physics::World::Step");
        world.Step(dt);
    }
    {
        FBZZ_PROFILE_SCOPE("PhysicsSystem::DispatchCollisionEvents");
        DispatchCollisionEvents(scene, world, colliderOwners);
    }

    for (auto [tf, rb] : scene.View<Transform, RigidBodyComponent>()) {
        if (!rb.enabled || !rb.rigidBody) continue;
        tf.localPosition = rb.rigidBody->GetPosition();
        tf.localRotation = rb.rigidBody->GetRotation();
        tf.position = tf.localPosition;
        tf.rotation = tf.localRotation;
    }
}

} // namespace fbzz::scene
