/// @file    PhysicsSystem.cpp
/// @brief   Scene と physics::World の同期。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// @brief RigidBodyComponent と ColliderComponent を physics に反映し、Step 後に Transform へ戻す。
/// @brief Scene から physics への依存方向を保つ。
#include "Engine/Scene/Systems/PhysicsSystem.hpp"
#include "Engine/Scene/Systems/ColliderSync.hpp"
#include "Engine/Scene/Systems/JointSync.hpp"
#include "Engine/Core/Scheduler/SystemContext.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/CharacterControllerComponent.hpp"
#include "Engine/Scene/Components/ColliderComponent.hpp"
#include "Engine/Scene/Components/JointComponent.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/RigidBodyComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Scene/Components/TerrainGridComponent.hpp"
#include "Engine/Scene/Components/VolumeComponent.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include "Engine/Scene/Components/WaterComponent.hpp"
#include "Engine/Scene/Fields/FlowFieldEval.hpp"
#include "Engine/Scene/Fields/FlowFieldFrame.hpp"
#include "Engine/Scene/ScriptComponent.hpp"
#include "Engine/Scene/Systems/WaterSystem.hpp"
#include <Engine/Core/Time.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Physics/ColliderVolume.hpp>
#include <Physics/ConvexHullCollider.hpp>
#include <Physics/FlowVolume.hpp>
#include <Physics/FluidVolume.hpp>
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

/// @brief physics::CollisionEvent は Collider* のペアを持つが Scene 側の情報を持たない。
/// @brief このマップで Collider* → (GameObject*, ColliderComponent*) を O(1) で逆引きし、
/// @brief Script へのコールバック発火で使う。
using ColliderOwnerMap = std::unordered_map<const physics::Collider*, ColliderOwner>;
using ScriptCollisionCallback = void (Script::*)(const CollisionInfo&);
/// @brief 剛体ごとのコライダー体積の合計。水の浮力が «体がどれだけ沈んだか» を測るのに使う。
using BodyVolumeMap = physics::BodyVolumeMap;

/// @brief Transform の直接編集は動的剛体に対するテレポート要求として扱う。
/// @brief 浮動小数の再計算誤差では履歴をリセットしないよう、位置と回転に小さい許容値を持たせる。
bool PoseChanged(const RigidBodyComponent& component, const Transform& transform)
{
    if (!component.hasPhysicsSyncState)
        return true;
    constexpr float POSITION_EPSILON_SQ = 1.0e-10f;
    constexpr float ROTATION_DOT_EPSILON = 1.0e-5f;
    const bool positionChanged =
        (transform.worldPosition - component.lastPhysicsPosition).LengthSq() >
        POSITION_EPSILON_SQ;
    const float rotationDot = std::abs(math::Quaternion::Dot(
        transform.worldRotation.Normalized(), component.lastPhysicsRotation));
    return positionChanged || (1.0f - rotationDot) > ROTATION_DOT_EPSILON;
}

/// @brief 水面 1 面ぶんを physics::FluidVolume の入力へ写す。
/// @note 法則そのものは Physics が持つ (physics::FluidVolume)。ここに残すのは配線だけ。
/// @note 表面の callback が WaterComponent を値で捕まえる理由: Volume は毎フレーム作り直す
///       ので値が古くなることはなく、物理ステップ中に Scene を触らずに済む。
/// @note 流速の callback は不変の場を shared_ptr で共有し、全 substep と全水面で同じ配列を読む。
/// @see Docs/design/buoyancy.md
/// @see Docs/design/water-waves.md 「流れの場が水面に出る 3 つの道」
physics::FluidVolumeDesc MakeWaterFluidDesc(const WaterComponent& water,
                                            const Transform& transform,
                                            std::shared_ptr<const std::vector<ActiveFlowField>> fields,
                                            std::shared_ptr<const physics::BodyRadiusMap> radii)
{
    physics::FluidVolumeDesc desc;
    desc.surfaceHeight = [snapshot = water](float worldX, float worldZ, float time) {
        return snapshot.GetSurfaceHeightAt(worldX, worldZ, time);
    };
    /// @note 流速は場所の関数。.mat の一様な current に、その点を覆う流れの場を足す。
    ///       これで浮いた物は flowCoupling を立てなくても Vortex の周りを回る。
    desc.flowVelocity = [snapshot = water, snapshotFields = fields,
                         baseY = transform.worldPosition.y,
                         time = Time::time](const math::Vector3& p) {
        return WaterFlowVelocityAt(snapshot, *snapshotFields, baseY, p, time);
    };
    desc.center     = transform.worldPosition;
    desc.halfX      = water.extentX * 0.5f * std::abs(transform.worldScale.x);
    desc.halfZ      = water.extentZ * 0.5f * std::abs(transform.worldScale.z);
    desc.buoyancy   = water.buoyancy;
    desc.drag       = water.waterDrag;
    desc.depthLimit = water.buoyancyDepth;
    desc.startTime  = Time::time;
    desc.surfaceHeightBound = water.SurfaceHeightBound();
    desc.radii      = std::move(radii);
    return desc;
}

/// @brief シーンの流れを physics::FlowVolume として 1 個だけ申告する。
/// @return 今フレームのハンドル。場が 1 本も無ければ無効ハンドル (EndSceneSync が枠を返す)。
/// @pre FlowFieldSystem (PrePhysics) がこの System より前に走ること。走らなくても
///      Scene::FlowFrame() が遅延更新するが、その場合は収集の時刻がフレーム内でぶれる。
/// @note flowVelocity は不変のスナップショットを共有し、物理ステップ中に Scene を参照しない。
///       場が 1 本も無いフレームは申告しない。無いことは «流速 0» ではなく «媒質について
///       何も言っていない» 意味で、0 を渡すと flowCoupling を立てた体が理由もなく減速する。
/// @see Docs/design/flow-field.md §9-6
physics::VolumeHandle SyncFlowVolume(Scene& scene, physics::World& world,
                                     physics::VolumeHandle handle)
{
    const FlowFieldFrame& frame = scene.FlowFrame();
    if (frame.fields->empty()) return {};

    physics::FlowVolumeDesc desc;
    /// @note channels は全ビット。剛体にはチャンネルの申告が無いので «どの場も受ける»。
    /// @note 時刻を捕まえ時に固定するのは、Curl のスクロールが substep ごとに進むと
    ///       同じフレームの中で場が動いてしまうため。
    desc.flowVelocity = MakeFlowSampler(frame, {}, Time::time);
    return world.SyncVolume(handle, std::make_unique<physics::FlowVolume>(std::move(desc)));
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

        /// @note TransformSystem は localPosition に親 worldScale を掛けてから親回転を適用する。
        ///       Physics は world pose を返すため、ここで同じ式を逆変換して local pose に戻す。
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

/// @brief AddColliderInstance — 構築・姿勢反映済みの Collider を World へ登録する。
/// @brief PRECONDITION: 呼び出し前に PrepareCollider() が成功していること (col.collider が有効)。
template<typename T>
void AddColliderInstance(Scene& scene,
                         GameObject& go,
                         T& col,
                         physics::World& world,
                         ColliderOwnerMap& colliderOwners,
                         BodyVolumeMap& bodyVolumes,
                         float dt)
{
    auto* rb = go.GetComponent<RigidBodyComponent>();
    physics::RigidBody* body = rb && rb->enabled && rb->rigidBody ? rb->rigidBody.get() : nullptr;

    /// @note 祖先の剛体へ属させる指定。自分に剛体が無いときだけ遡る。既定では遡らない —
    ///       «親が剛体・子は静的な床» の構成もあるため、遡るかはコライダー側の申告に任せ、
    ///       立てた所だけが «同じ体の一部» になる。質量も遡らせない。下の FromDensity は
    ///       «この剛体の体積» を積むので、子の当たり (骨のヒットボックス等) まで数えると
    ///       形を 1 つ足すたびに質量が勝手に増える。質量は自分の GameObject の分だけ。
    bool attachedToAncestor = false;
    if (!body && col.attachToParentBody) {
        for (GameObject* parent = go.GetParent(); parent; parent = parent->GetParent()) {
            auto* parentRb = parent->GetComponent<RigidBodyComponent>();
            if (parentRb && parentRb->enabled && parentRb->rigidBody) {
                body = parentRb->rigidBody.get();
                attachedToAncestor = true;
                break;
            }
        }
    }

    /// @note 共有 .physmat を参照しているなら、この時点で実効値へ解決する。毎フレーム解決する
    ///       のは、エディタで .physmat を編集した結果を参照コライダーへ待ち時間なく反映させる
    ///       ため。未参照なら即 return、参照ありでも AssetManager のパスキャッシュ引きだけなので
    ///       フレームコストは実質ゼロ。
    col.ResolvePhysicsMaterial();

    /// @note 密度モードの剛体へ、このコライダーぶんの質量を積む。
    if (rb && rb->massMode == MassMode::FromDensity && col.collider)
        rb->computedMass += col.collider->ComputeVolume() * col.material.density;

    /// @note 剛体を持つコライダーの姿勢は World::UpdateColliders が
    ///       «body の位置 + body の回転 × centerOffset» で組む。剛体が自分の GameObject に
    ///       乗っているときは body の姿勢 = 自分の姿勢なので、centerOffset はローカルの
    ///       center そのままでよい。祖先の剛体へ属させたときは両者がずれるので、
    ///       «body から見た自分» へ直さないと、当たりが親の位置に生えてしまう。
    ///
    ///       ⚠ 回転は body のものが使われる。祖先へ属させてよいのは、向きが剛体と揃っている
    ///       当たりだけ (骨と一緒に回る当たりは、この経路では正しく回らない)。
    math::Vector3 centerOffset = ColliderCenterOffset(go, col);
    if (attachedToAncestor && body) {
        centerOffset = body->GetRotation().Inverse()
                     * (ColliderWorldCenter(go, col) - body->GetPosition());
    }
    physics::ColliderInstance instance{ col.collider.get(), body, &col.material, centerOffset, col.isTrigger, go.layer };
    col.colliderHandle = world.SyncCollider(col.colliderHandle, instance);
    colliderOwners[col.collider.get()] = { &go, &col };
    /// @note トリガーは体の一部ではないので、浸かる大きさには数えない。
    if (body && !col.isTrigger)
        bodyVolumes[body] += col.collider->ComputeVolume();

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
        settings.explosionImpulse = volume->explosionImpulse;
        settings.timeScale = volume->timeScale;
        settings.duration = volume->duration;

        volume->volumeHandle = world.SyncVolume(
            volume->volumeHandle,
            std::make_unique<physics::ColliderVolume>(col.collider.get(), settings));
    }
}

template<typename T>
void SyncColliderComponents(Scene& scene,
                            physics::World& world,
                            ColliderOwnerMap& colliderOwners,
                            BodyVolumeMap& bodyVolumes,
                            float dt)
{
    /// @note GameObject 全体を毎 fixed step 走査して各 Collider 型を GetComponent すると、
    ///       物理を持たないオブジェクト数に比例して固定コストが増える。ComponentArray が
    ///       保持する Entity span を入口にし、存在する Collider component だけを同期する。
    for (EntityID id : scene.GetEntities<T>()) {
        GameObject* go = scene.GetGameObject(id);
        T* col = scene.GetComponent<T>(id);
        if (!go || !go->activeInHierarchy() || !col || !col->enabled) continue;

        /// @note 構築 → 形状同期 → 姿勢反映は ColliderSync に集約。
        ///       同じ手順をコライダー可視化 (DebugCollidersPass) も使うため、両者の見え方が一致する。
        if (!PrepareCollider(scene, *go, *col)) continue;

        AddColliderInstance(scene, *go, *col, world, colliderOwners, bodyVolumes, dt);
    }
}

/// @brief event の向きは A → B で固定されているため、B 側のスクリプトへ渡すときは
/// @brief 法線と相対速度を反転させて「自分から見た相手」に揃える。
/// @brief approachSpeed / normalImpulse はスカラーで、両者を同時に反転すると
/// @brief 内積の符号が変わらないため、どちら側でも同じ値になる。
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

/// @brief CharacterController は Script の有無にかかわらず物理接触を受け取る。
/// @note 接地判定のためだけに全キャラクターへ同じ OnCollisionStay 実装を要求すると、
///       スクリプトを使わない敵・NPC・プレハブが成立しない。
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

}

ComponentAccess PhysicsSystem::GetAccess() const
{
    return ComponentAccess{}
        .Reads<ColliderComponent, RigidBodyComponent>()
        .Writes<RigidBodyComponent, CharacterControllerComponent, VolumeComponent, WaterComponent,
                JointComponent>();
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
        scene.GetEntities<CylinderColliderComponent>().size() +
        scene.GetEntities<MeshColliderComponent>().size() +
        scene.GetEntities<ConvexHullColliderComponent>().size() +
        scene.GetEntities<TerrainColliderComponent>().size());
    static thread_local BodyVolumeMap bodyVolumes;
    bodyVolumes.clear();

    world.BeginSceneSync();

    {
        FBZZ_PROFILE_SCOPE("PhysicsSystem::SyncRigidBodies");
        for (EntityID id : scene.GetEntities<RigidBodyComponent>()) {
            GameObject* go = scene.GetGameObject(id);
            auto* rb = scene.GetComponent<RigidBodyComponent>(id);
            if (!go || !go->activeInHierarchy() || !rb || !rb->enabled || !rb->rigidBody) continue;

            /// @note BodyHandle は Component 側へ永続化される runtime state で、Component 実体へ
            ///       直接書き戻す。動的剛体は Physics を正とし、Transform が前回物理姿勢から
            ///       明示的に変わった時だけテレポートとして Scene → Physics へ送る (毎 step の
            ///       無条件再送は補間履歴と FixedScript の直接操作を巻き戻すため行わない)。
            if (rb->rigidBody->IsStatic() || PoseChanged(*rb, go->transform)) {
                rb->rigidBody->SetPosition(go->transform.worldPosition);
                rb->rigidBody->SetRotation(go->transform.worldRotation);
                rb->ResetPhysicsSyncState(
                    go->transform.worldPosition, go->transform.worldRotation);
            }
            /// @note 毎フレーム押し込む理由: 正本はコンポーネント側。Inspector と
            ///       スクリプトが触るのはそちらなので、massMode と同じ形で剛体へ配る。
            rb->rigidBody->SetFlowCoupling(rb->flowCoupling);
            rb->bodyHandle = world.SyncBody(rb->bodyHandle, rb->rigidBody.get());
        }
    }

    /// @note 密度から質量を出す剛体は、コライダー同期の中で体積 × 密度を積算するため、ここで
    ///       累算器をゼロに戻す。コライダー側で積むのは、1 つの GameObject に複数のコライダーが
    ///       付くことがあり (胴 + 頭のカプセル等) 質量は全部の体積の合計であるべきで、コライダー
    ///       走査はどのみち毎フレーム行うのでそこに相乗りするのが最も安いため。
    for (EntityID id : scene.GetEntities<RigidBodyComponent>()) {
        auto* rb = scene.GetComponent<RigidBodyComponent>(id);
        if (rb && rb->massMode == MassMode::FromDensity) rb->computedMass = 0.0f;
    }

    {
        FBZZ_PROFILE_SCOPE("PhysicsSystem::SyncColliders");
        SyncColliderComponents<AabbColliderComponent>(scene, world, colliderOwners, bodyVolumes, dt);
        SyncColliderComponents<BoxColliderComponent>(scene, world, colliderOwners, bodyVolumes, dt);
        SyncColliderComponents<SphereColliderComponent>(scene, world, colliderOwners, bodyVolumes, dt);
        SyncColliderComponents<CapsuleColliderComponent>(scene, world, colliderOwners, bodyVolumes, dt);
        SyncColliderComponents<CylinderColliderComponent>(scene, world, colliderOwners, bodyVolumes, dt);
        SyncColliderComponents<MeshColliderComponent>(scene, world, colliderOwners, bodyVolumes, dt);
        SyncColliderComponents<ConvexHullColliderComponent>(scene, world, colliderOwners, bodyVolumes, dt);
        SyncColliderComponents<TerrainColliderComponent>(scene, world, colliderOwners, bodyVolumes, dt);
    }

    {
        FBZZ_PROFILE_SCOPE("PhysicsSystem::SyncWaterVolumes");
        std::shared_ptr<const physics::BodyRadiusMap> radii;
        for (EntityID id : scene.GetEntities<WaterComponent>()) {
            auto* water = scene.GetComponent<WaterComponent>(id);
            GameObject* go = scene.GetGameObject(id);
            if (!water || !go || !go->activeInHierarchy() || !water->enabled || !water->buoyancyEnabled)
                continue;
            /// @note 半径表は水面が 1 枚でもあるときだけ作り、全水面で共有する。
            if (!radii) radii = physics::MakeBodyRadii(bodyVolumes);
            water->volumeHandle = world.SyncVolume(
                water->volumeHandle,
                std::make_unique<physics::FluidVolume>(
                    MakeWaterFluidDesc(*water, go->transform, scene.FlowFrame().fields, radii)));
        }
    }

    {
        FBZZ_PROFILE_SCOPE("PhysicsSystem::SyncFlowVolume");
        m_flowVolumeHandle = SyncFlowVolume(scene, world, m_flowVolumeHandle);
    }

    /// @note 関節は剛体と同じ «毎フレーム申告» で寿命を持つ。申告が途切れた制約は
    ///       EndSceneSync が破棄するので、Scene 側に «外す» 経路は要らない。
    SyncJointComponents(scene, world);

    {
        FBZZ_PROFILE_SCOPE("PhysicsSystem::ApplyDensityMass");
        for (EntityID id : scene.GetEntities<RigidBodyComponent>()) {
            auto* rb = scene.GetComponent<RigidBodyComponent>(id);
            GameObject* go = scene.GetGameObject(id);
            if (!go || !go->activeInHierarchy() || !rb ||
                rb->massMode != MassMode::FromDensity || !rb->rigidBody) continue;
            /// @note コライダーが 1 つも付いていない (= 体積 0) 剛体を質量 0 にすると
            ///       invMass が無限大になり、わずかな接触で吹き飛ぶ。下限で守る。
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
            rb->CommitPhysicsSyncState(position, rotation);
            WriteWorldPoseToTransform(*go, position, rotation);
        }
    }
}

}
