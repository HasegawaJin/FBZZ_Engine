// FBZZ Engine
// PhysicsSystem.cpp | fbzz::scene
// Scene と physics::World の同期
// RigidBodyComponent と ColliderComponent を physics に反映し、Step 後に Transform へ戻す。
// Scene から physics への依存方向を保つ。
#include "Engine/Scene/Systems/PhysicsSystem.hpp"
#include "Engine/Scene/Systems/ColliderSync.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/CharacterControllerComponent.hpp"
#include "Engine/Scene/Components/ColliderComponent.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/RigidBodyComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Scene/Components/TerrainGridComponent.hpp"
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

// Transform の直接編集は動的剛体に対するテレポート要求として扱う。
// 浮動小数の再計算誤差では履歴をリセットしないよう、位置と回転に小さい許容値を持たせる。
bool PoseChanged(const RigidBodyComponent& component, const Transform& transform)
{
    if (!component.hasPhysicsPoseHistory)
        return true;
    constexpr float POSITION_EPSILON_SQ = 1.0e-10f;
    constexpr float ROTATION_DOT_EPSILON = 1.0e-5f;
    const bool positionChanged =
        (transform.worldPosition - component.currentPhysicsPosition).LengthSq() >
        POSITION_EPSILON_SQ;
    const float rotationDot = std::abs(math::Quaternion::Dot(
        transform.worldRotation.Normalized(), component.currentPhysicsRotation));
    return positionChanged || (1.0f - rotationDot) > ROTATION_DOT_EPSILON;
}

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

// AddColliderInstance — 構築・姿勢反映済みの Collider を World へ登録する。
// PRECONDITION: 呼び出し前に PrepareCollider() が成功していること (col.collider が有効)。
template<typename T>
void AddColliderInstance(Scene& scene,
                         GameObject& go,
                         T& col,
                         physics::World& world,
                         ColliderOwnerMap& colliderOwners,
                         float dt)
{
    auto* rb = go.GetComponent<RigidBodyComponent>();
    physics::RigidBody* body = rb && rb->enabled && rb->rigidBody ? rb->rigidBody.get() : nullptr;

    // 共有 .physmat を参照しているなら、この時点で実効値へ解決する。
    // WHY 毎フレームか: エディタで .physmat を編集した結果を、参照している全コライダーへ
    //     待ち時間なく反映させるため。未参照なら即 return、参照ありでも AssetManager の
    //     パスキャッシュ引きだけなので、フレームコストは実質ゼロ。
    col.ResolvePhysicsMaterial();

    // 密度モードの剛体へ、このコライダーぶんの質量を積む。
    if (rb && rb->massMode == MassMode::FromDensity && col.collider)
        rb->computedMass += col.collider->ComputeVolume() * col.material.density;

    const math::Vector3 centerOffset = ColliderCenterOffset(go, col);
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
        if (!go || !go->activeInHierarchy() || !col || !col->enabled) continue;

        // 構築 → 形状同期 → 姿勢反映は ColliderSync に集約。
        // 同じ手順をコライダー可視化 (DebugCollidersPass) も使うため、両者の見え方が一致する。
        if (!PrepareCollider(scene, *go, *col)) continue;

        AddColliderInstance(scene, *go, *col, world, colliderOwners, dt);
    }
}

// event の向きは A → B で固定されているため、B 側のスクリプトへ渡すときは
// 法線と相対速度を反転させて「自分から見た相手」に揃える。
// approachSpeed / normalImpulse はスカラーで、両者を同時に反転すると
// 内積の符号が変わらないため、どちら側でも同じ値になる。
CollisionInfo BuildCollisionInfo(const ColliderOwner& self,
                                 const ColliderOwner& other,
                                 const physics::CollisionEvent& event,
                                 bool flipped)
{
    const float sign = flipped ? -1.0f : 1.0f;

    CollisionInfo info;
    info.self = self.gameObject;
    info.other = other.gameObject;
    info.selfCollider = self.collider;
    info.otherCollider = other.collider;
    info.contactNormal = event.normal * sign;
    info.contactPoint = event.point;
    info.contactDepth = event.depth;
    info.relativeVelocity = event.relativeVelocity * sign;
    info.approachSpeed = event.approachSpeed;
    info.impactImpulse = event.normalImpulse;
    return info;
}

// CharacterController は Script の有無にかかわらず物理接触を受け取る。
// WHY: 接地判定のためだけに全キャラクターへ同じ OnCollisionStay 実装を要求すると、
//      スクリプトを使わない敵・NPC・プレハブが成立しないため。
void RegisterCharacterGroundContact(const ColliderOwner& self,
                                    const ColliderOwner& other,
                                    const physics::CollisionEvent& event,
                                    bool flipped)
{
    if (!self.gameObject || !other.gameObject) return;

    auto* controller = self.gameObject->GetComponent<CharacterControllerComponent>();
    if (!controller) return;

    controller->RegisterGroundContact(BuildCollisionInfo(self, other, event, flipped));
}

void DispatchToScript(Scene& scene,
                      const ColliderOwner& self,
                      const ColliderOwner& other,
                      const physics::CollisionEvent& event,
                      bool flipped,
                      ScriptCollisionCallback callback)
{
    if (!self.gameObject || !other.gameObject) return;

    auto* scriptComponent = self.gameObject->GetComponent<ScriptComponent>();
    if (!scriptComponent) return;

    CollisionInfo info = BuildCollisionInfo(self, other, event, flipped);

    for (auto& entry : scriptComponent->scripts) {
        if (!entry.script || !entry.script->enabled) continue;
        entry.script->SetContext(&scene, self.gameObject);
        entry.script->ExecuteCallback(callback, info, "collision callback");
    }
}

void DispatchCollisionEvent(Scene& scene,
                            const ColliderOwnerMap& owners,
                            const physics::CollisionEvent& event,
                            ScriptCollisionCallback callback,
                            bool registerGroundContact)
{
    auto ownerA = owners.find(event.colliderA);
    auto ownerB = owners.find(event.colliderB);
    if (ownerA == owners.end() || ownerB == owners.end()) return;

    if (registerGroundContact && !event.isTrigger) {
        RegisterCharacterGroundContact(ownerA->second, ownerB->second, event, false);
        RegisterCharacterGroundContact(ownerB->second, ownerA->second, event, true);
    }

    DispatchToScript(scene, ownerA->second, ownerB->second, event, false, callback);
    DispatchToScript(scene, ownerB->second, ownerA->second, event, true,  callback);
}

void DispatchCollisionEvents(Scene& scene,
                             physics::World& world,
                             const ColliderOwnerMap& owners)
{
    for (const auto& event : world.GetEnterEvents())
    {
        DispatchCollisionEvent(scene, owners, event, event.isTrigger
            ? &Script::OnTriggerEnter
            : &Script::OnCollisionEnter, true);
    }

    for (const auto& event : world.GetStayEvents())
    {
        DispatchCollisionEvent(scene, owners, event, event.isTrigger
            ? &Script::OnTriggerStay
            : &Script::OnCollisionStay, true);
    }

    for (const auto& event : world.GetExitEvents())
    {
        DispatchCollisionEvent(scene, owners, event, event.isTrigger
            ? &Script::OnTriggerExit
            : &Script::OnCollisionExit, false);
    }
}

} // namespace

ComponentAccess PhysicsSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<ColliderComponent, RigidBodyComponent>()
        .Writes<RigidBodyComponent, CharacterControllerComponent, VolumeComponent>();
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
            if (!go || !go->activeInHierarchy() || !rb || !rb->enabled || !rb->rigidBody) continue;

            // WHY: BodyHandle は Component 側へ永続化される runtime state。
            //      SceneView の structured binding に依存せず、Component 実体へ直接書き戻す。
            // 動的剛体は Physics を正とし、Transform が前回物理姿勢から明示的に変わった時だけ
            // テレポートとして Scene → Physics へ送る。毎 step の無条件再送は補間履歴と
            // FixedScript からの直接的な剛体操作を巻き戻すため削除する。
            if (rb->rigidBody->IsStatic() || PoseChanged(*rb, go->transform)) {
                rb->rigidBody->SetPosition(go->transform.worldPosition);
                rb->rigidBody->SetRotation(go->transform.worldRotation);
                rb->ResetPhysicsPoseHistory(
                    go->transform.worldPosition, go->transform.worldRotation);
            }
            rb->BeginPhysicsStep();
            rb->bodyHandle = world.SyncBody(rb->bodyHandle, rb->rigidBody.get());
        }
    }

    // 密度から質量を出す剛体は、コライダー同期の中で体積 × 密度を積算する。
    // ここでその累算器をゼロに戻す。
    // WHY コライダー側で積むか: 1 つの GameObject に複数のコライダーが付くことがあり
    //     (胴 + 頭のカプセル等)、質量は「全部の体積の合計」であるべきだから。
    //     コライダーの走査はどのみち毎フレーム行うので、そこに相乗りするのが最も安い。
    for (EntityID id : scene.GetEntities<RigidBodyComponent>()) {
        auto* rb = scene.GetComponent<RigidBodyComponent>(id);
        if (rb && rb->massMode == MassMode::FromDensity) rb->computedMass = 0.0f;
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
        FBZZ_PROFILE_SCOPE("PhysicsSystem::ApplyDensityMass");
        for (EntityID id : scene.GetEntities<RigidBodyComponent>()) {
            auto* rb = scene.GetComponent<RigidBodyComponent>(id);
            GameObject* go = scene.GetGameObject(id);
            if (!go || !go->activeInHierarchy() || !rb ||
                rb->massMode != MassMode::FromDensity || !rb->rigidBody) continue;
            // コライダーが 1 つも付いていない (= 体積 0) 剛体を質量 0 にすると
            // invMass が無限大になり、わずかな接触で吹き飛ぶ。下限で守る。
            constexpr float MIN_MASS = 0.001f;
            rb->rigidBody->SetMass(std::max(rb->computedMass, MIN_MASS));
        }
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
            if (!go || !go->activeInHierarchy() || !rb || !rb->enabled || !rb->rigidBody) continue;
            const math::Vector3 position = rb->rigidBody->GetPosition();
            const math::Quaternion rotation = rb->rigidBody->GetRotation();
            rb->CommitPhysicsPose(position, rotation);
            WriteWorldPoseToTransform(*go, position, rotation);
        }
    }
}

} // namespace fbzz::scene
