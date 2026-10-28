/// @file    ManagedServicesBridgeTests.cpp
/// @brief   Component サービス実転送・形状寿命・フィルター・原子性・カメラ座標契約の回帰。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Scene/SceneManager.hpp>
#include <Engine/Core/Cursor.hpp>
#include <Engine/Core/Application.hpp>
#include <Engine/Util/Random.hpp>
#include <Engine/Util/SaveStore.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/JointComponent.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Physics/World.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedPhysicsBridge.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedServicesBridge.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedObjectControls.hpp"
#include <array>
#include <bit>
#include <charconv>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>
#include <fstream>

namespace fbzz::tests {
namespace {

/// @note Tests author through the same owned draft/publication path as production; no mutable stored alias is exposed.
template<typename Owner, typename Change>
void EditMaterialAuthoring(Owner& owner, Change&& change)
{
    auto edit = owner.BeginAuthoringEdit();
    change(edit.Draft());
    EXPECT_NE(edit.Commit(owner), asset::detail::MaterialAuthoringCommitResult::CONFLICT);
}


class RuntimeServiceScope final {
public:
    explicit RuntimeServiceScope(scene::ScriptRuntime& runtime)
        : m_previous(scene::ScriptRuntime::GetOverride()), m_play(scene::Script::IsInPlayMode()), m_suppressed(core::Cursor::IsSuppressed())
    { scene::ScriptRuntime::Override(&runtime); scene::Script::SetInPlayMode(true); core::Cursor::SetSuppressed(true); }
    ~RuntimeServiceScope()
    { core::Cursor::ClearRequests(); core::Cursor::SetSuppressed(m_suppressed); scene::ScriptRuntime::Override(m_previous); scene::Script::SetInPlayMode(m_play); }
private:
    scene::ScriptRuntime* m_previous;
    bool m_play;
    bool m_suppressed;
};
} /// @note namespace
class ManagedServicesBridgeTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    scene::Script m_caller;
    scene::managed_detail::ManagedServicesBridge m_services;
    scene::managed_detail::ManagedPhysicsBridge m_physics;
    scene::managed_detail::ManagedServiceResult m_result;

    template<typename T> static void Append(std::string& bytes, const T& value)
    { bytes.append(std::bit_cast<const char*>(&value), sizeof(value)); }
    scene::ManagedBridgeStatus Run(scene::GameObject& target, scene::ManagedBridgeOp operation, uint32_t selector,
        bool readOnly, int64_t integer = 0, std::initializer_list<float> values = {},
        std::initializer_list<std::string> text = {}, scene::GameObject* related = nullptr, bool fixed = true, bool collision = false)
    {
        std::string bytes; Append(bytes, related ? uint64_t{1} : uint64_t{0}); Append(bytes, integer);
        Append(bytes, static_cast<uint32_t>(values.size())); Append(bytes, static_cast<uint32_t>(text.size()));
        for (float value : values) Append(bytes, value);
        for (const auto& value : text) { Append(bytes, static_cast<uint32_t>(value.size())); bytes += value; }
        scene::ManagedBridgeRequest request; request.op = operation; request.index = selector;
        request.flags = readOnly ? 1u : 0u; request.generation = 7; request.ownerId = 9;
        request.instanceId = 11; request.callId = 13;
        request.input = bytes.data(); request.inputBytes = static_cast<uint32_t>(bytes.size());
        return m_services.Run(m_scene, m_caller, target, related, request, m_result, fixed, collision);
    }
    scene::ManagedBridgeStatus StorePacket(scene::GameObject& target, uint32_t selector, uint64_t token,
        uint64_t offset = 0, std::string_view chunk = {}, uint64_t call = 13, uint64_t owner = 9, uint64_t generation = 7)
    {
        std::string bytes; Append(bytes, token);
        if (selector == 23) { Append(bytes, offset); Append(bytes, static_cast<uint32_t>(chunk.size())); Append(bytes, uint32_t{0}); bytes.append(chunk); }
        scene::ManagedBridgeRequest request; request.op = scene::ManagedBridgeOp::SERVICE_SAVE; request.index = selector;
        request.generation = generation; request.ownerId = owner; request.instanceId = 11; request.callId = call;
        request.input = bytes.data(); request.inputBytes = static_cast<uint32_t>(bytes.size());
        return m_services.Run(m_scene, m_caller, target, nullptr, request, m_result, false);
    }
    scene::RigidBodyComponent& Body(scene::GameObject& object)
    {
        auto& component = object.AddComponent<scene::RigidBodyComponent>();
        component.rigidBody = std::make_unique<physics::RigidBody>(); component.rigidBody->m_useGravity = false;
        return component;
    }
};

namespace {
/// @note Frozen Scene-wide traversal preserves first equal-height hit and false output semantics.
bool ReferenceSceneTerrain(scene::Scene& queryScene,const math::Vector3& reference,float distance,bool ceiling,scene::TerrainSurfaceSample& out)
{
    bool found=false;scene::TerrainSurfaceSample best;
    for (auto& object:queryScene.GameObjects()) {
        if (!object.activeInHierarchy()) continue;
        const auto* terrain=object.GetComponent<scene::TerrainComponent>();
        if (!terrain || !terrain->enabled) continue;
        scene::TerrainSurfaceSample sample;
        const bool hit=ceiling ? scene::TrySampleTerrainCeilingWorld(*terrain,object.transform,reference,distance,sample)
            : scene::TrySampleTerrainFloorWorld(*terrain,object.transform,reference,distance,sample);
        if (hit && (!found || (ceiling ? sample.position.y<best.position.y : sample.position.y>best.position.y))) {best=sample;found=true;}
    }
    if (!found)return false;
    out=best;return true;
}
void ExpectSceneTerrainEquivalent(scene::Scene& queryScene,scene::Script& caller,const math::Vector3& reference)
{
    for (bool ceiling:{false,true}) for (float distance:std::array<float,4>{0,1,10,-1}) {
        const scene::TerrainSurfaceSample sentinel{{77,88,99},{1,2,3}};
        auto old=sentinel,actual=sentinel;
        const bool oldHit=ReferenceSceneTerrain(queryScene,reference,distance,ceiling,old);
        const bool newHit=ceiling ? caller.scene.TryGetTerrainCeilingAt(reference,distance,actual.position,actual.normal)
            : caller.scene.TryGetTerrainFloorAt(reference,distance,actual.position,actual.normal);
        ASSERT_EQ(newHit,oldHit);EXPECT_VEC3_NEAR(actual.position,old.position,1e-5f);EXPECT_VEC3_NEAR(actual.normal,old.normal,1e-5f);
    }
}
void SetQueryPlanes(scene::TerrainComponent& terrain,float slope)
{
    terrain.columns=terrain.rows=2;terrain.cellSize=2;terrain.InitFlat();
    ASSERT_TRUE(terrain.SetHoleCell(0,0,true));
    ASSERT_TRUE(terrain.SetSurfaceGeometry({{0,-6,0},{0,-6,2},{2,-6+slope,0},{2,-6+slope,2},
        {0,-2,0},{2,-2+slope,0},{0,-2,2},{2,-2+slope,2}}, {0,1,2,2,1,3,4,5,6,6,5,7}));
}
} /// @note namespace
TEST_F(ManagedServicesBridgeTest, SceneCaveQueriesSelectNearestActiveSurfacesWithoutRegisteredManagerOrPhysics)
{
    scene::ScriptRuntime runtime; RuntimeServiceScope scope(runtime);
    scene::Script::SetInPlayMode(false);
    auto& caller = m_scene.CreateGameObject("Cave observer"); m_caller.SetContext(&m_scene, &caller);
    auto& cave = m_scene.CreateGameObject("Cave");
    auto& terrain = cave.AddComponent<scene::TerrainComponent>();
    terrain.columns = terrain.rows = 2; terrain.cellSize = 2; terrain.InitFlat();
    ASSERT_TRUE(terrain.SetHoleCell(0,0,true));
    ASSERT_TRUE(terrain.SetSurfaceGeometry(
        {{0,-6,0},{0,-6,2},{2,-6,0},{2,-6,2},{0,-2,0},{2,-2,0},{0,-2,2},{2,-2,2}},
        {0,1,2,2,1,3,4,5,6,6,5,7}));
    auto& ledge = m_scene.CreateGameObject("Near floor");
    auto& ledgeTerrain = ledge.AddComponent<scene::TerrainComponent>();
    ledgeTerrain.columns = ledgeTerrain.rows = 2; ledgeTerrain.cellSize = 2; ledgeTerrain.InitFlat();
    ledge.transform.worldPosition.y = -5;
    ASSERT_EQ(Run(caller, scene::ManagedBridgeOp::SERVICE_SCENE, 2, true, 0, {.5f,-4,.5f,10}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_result.integer, 1); ASSERT_EQ(m_result.values.size(), 6U); EXPECT_FLOAT_EQ(m_result.values[1], -5);
    ledgeTerrain.enabled = false;
    ASSERT_EQ(Run(caller, scene::ManagedBridgeOp::SERVICE_SCENE, 2, true, 0, {.5f,-4,.5f,10}), scene::ManagedBridgeStatus::OK);
    EXPECT_FLOAT_EQ(m_result.values[1], -6); EXPECT_GT(m_result.values[4], .99f);
    ASSERT_EQ(Run(caller, scene::ManagedBridgeOp::SERVICE_SCENE, 3, true, 0, {.5f,-4,.5f,10}), scene::ManagedBridgeStatus::OK);
    EXPECT_FLOAT_EQ(m_result.values[1], -2); EXPECT_LT(m_result.values[4], -.99f);
    ASSERT_EQ(Run(caller, scene::ManagedBridgeOp::SERVICE_SCENE, 2, true, 0, {.5f,-4,.5f,1}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_result.integer, 0); EXPECT_TRUE(m_result.values.empty());
    m_result.integer = 77;
    EXPECT_EQ(Run(caller, scene::ManagedBridgeOp::SERVICE_SCENE, 3, true, 0, {.5f,-4,.5f,-1}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(Run(caller, scene::ManagedBridgeOp::SERVICE_SCENE, 2, false, 0, {.5f,-4,.5f,10}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(m_result.integer, 77);
    m_caller.SetContext(nullptr, nullptr);
}
TEST_F(ManagedServicesBridgeTest, LightAndParticleUseRealRowsAndRejectInvalidMutationAtomically)
{
    scene::ScriptRuntime runtime; RuntimeServiceScope scope(runtime);
    auto& object = m_scene.CreateGameObject();
    auto& light = object.AddComponent<scene::LightComponent>();
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_LIGHT, 11, false, 0, {10, 45}), scene::ManagedBridgeStatus::OK);
    EXPECT_FLOAT_EQ(light.innerCone, 10); EXPECT_FLOAT_EQ(light.outerCone, 45);
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_LIGHT, 11, false, 0, {50, 30}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_FLOAT_EQ(light.innerCone, 10); EXPECT_FLOAT_EQ(light.outerCone, 45);
    scene::Script::SetInPlayMode(false);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_LIGHT, 9, false, 0, {6}), scene::ManagedBridgeStatus::OK);
    EXPECT_FLOAT_EQ(light.intensity, 6);
    auto& particle = object.AddComponent<scene::ParticleEmitter>();
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_PARTICLE, 6, false, 1), scene::ManagedBridgeStatus::INVALID_SCOPE);
    scene::Script::SetInPlayMode(true);
    particle.runtime.playTime = 12;
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_PARTICLE, 6, false, 1), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(particle.settings.playing); EXPECT_FLOAT_EQ(particle.runtime.playTime, 0);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_PARTICLE, 8, false, 17), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(particle.runtime.burstPending, 17);
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_PARTICLE, 8, false, std::numeric_limits<int>::max()), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(particle.runtime.burstPending, 17);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_PARTICLE, 7, false, 1), scene::ManagedBridgeStatus::OK);
    EXPECT_FALSE(particle.settings.playing); EXPECT_EQ(particle.runtime.burstPending, 0); EXPECT_TRUE(particle.runtime.gpuClearPending);
    bool exists = false;
    ASSERT_EQ(scene::managed_detail::ManagedObjectControls::Component(object, 28, scene::ManagedBridgeOp::COMPONENT_REMOVE, exists), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_PARTICLE, 0, true), scene::ManagedBridgeStatus::NOT_FOUND);
}

TEST_F(ManagedServicesBridgeTest, CursorRequestsKeepPriorityAndCleanupOnlyTheirOwnedLifetime)
{
    scene::ScriptRuntime runtime; RuntimeServiceScope scope(runtime);
    auto& object = m_scene.CreateGameObject(); m_caller.SetContext(&m_scene, &object);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CURSOR, 0, false, 9, {2, 0}, {"100"}), scene::ManagedBridgeStatus::OK);
    const int64_t first = m_result.integer;
    auto& other = m_scene.CreateGameObject();
    ASSERT_EQ(Run(other, scene::ManagedBridgeOp::SERVICE_CURSOR, 3, true, first, {}, {"9"}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_result.integer, 0); EXPECT_EQ(core::Cursor::GetRequestCount(), 1U);
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::Locked);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CURSOR, 0, false, 9, {1, 1}, {"200"}), scene::ManagedBridgeStatus::OK);
    const int64_t second = m_result.integer;
    EXPECT_NE(first, second); EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::Confined); EXPECT_TRUE(core::Cursor::IsVisible());
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CURSOR, 1, false, second, {0, 1}, {"9"}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::None);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CURSOR, 2, false, second, {}, {"9"}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(core::Cursor::GetLockMode(), core::CursorLockMode::Locked);
    m_services.CancelCursorOwner(9);
    EXPECT_EQ(core::Cursor::GetRequestCount(), 0U);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CURSOR, 3, true, first, {}, {"9"}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_result.integer, 0);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CURSOR, 0, false, 9, {1, 1}, {"100"}), scene::ManagedBridgeStatus::OK);
    const int64_t current = m_result.integer;
    EXPECT_NE(current, first); m_services.RetireGeneration(7); EXPECT_EQ(core::Cursor::GetRequestCount(), 0U);
}

TEST_F(ManagedServicesBridgeTest, SceneQueuesRegisteredNameAndQuitUsesAHostLifetimeIntent)
{
    scene::SceneManager manager; manager.SetScene(&m_scene);
    int requests = 0;
    uint64_t requestedLifetime = 0;
    struct QuitState { int* requests; uint64_t* lifetime; } quit{&requests, &requestedLifetime};
    scene::ScriptRuntime runtime; runtime.sceneManager = &manager;
    runtime.quitContext = &quit;
    runtime.requestQuit = [](void* context, uint64_t lifetime) {
        auto& state = *static_cast<QuitState*>(context); ++*state.requests; *state.lifetime = lifetime;
    };
    RuntimeServiceScope scope(runtime);
    auto& object = m_scene.CreateGameObject(); const auto originalId = object.GetID();
    int factories = 0;
    manager.Register("Next", [&] { ++factories; return std::make_unique<scene::Scene>(); });
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_SCENE, 1, false, 0, {}, {"Next"}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_result.integer, 1); EXPECT_EQ(factories, 0); EXPECT_EQ(manager.GetActive(), &m_scene);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_SCENE, 0, true), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(m_result.text.size(), 1U); EXPECT_TRUE(m_result.text[0].empty());
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_SCENE, 1, false, 0, {}, {"Missing"}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_result.integer, 0); EXPECT_EQ(factories, 0);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_APPLICATION, 4, false), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(requests, 1); EXPECT_EQ(requestedLifetime, m_scene.GetRenderSceneGeneration());
    EXPECT_NE(m_scene.GetGameObject(originalId), nullptr);
    scene::Script::SetInPlayMode(false);
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_APPLICATION, 4, false), scene::ManagedBridgeStatus::INVALID_SCOPE);
    EXPECT_EQ(requests, 1);
    manager.ReleaseOwnedScene(); physics::World world; manager.SetSimulating(false); manager.Update(0, world);
    EXPECT_EQ(factories, 0); EXPECT_EQ(manager.GetActive(), &m_scene);
    manager.SetScene(nullptr);
}

TEST_F(ManagedServicesBridgeTest, AssetServiceUsesSceneManagerPrefetchAndProgressContracts)
{
    scene::SceneManager manager; manager.SetScene(&m_scene);
    scene::ScriptRuntime runtime; runtime.sceneManager = &manager;
    RuntimeServiceScope scope(runtime);
    auto& object = m_scene.CreateGameObject("AssetOwner"); m_caller.SetContext(&m_scene, &object);
    int factories = 0;
    manager.Register("Next", [&] { ++factories; return std::make_unique<scene::Scene>(); });
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_ASSETS, 5, false, 0, {}, {"Next"}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_result.integer, 0); EXPECT_EQ(factories, 0);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_ASSETS, 6, true, 0, {}, {"Next"}), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(m_result.values.size(), 1U); EXPECT_FLOAT_EQ(m_result.values[0], 0.0f);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_ASSETS, 5, false, 0, {}, {"Missing"}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_result.integer, 0);
    manager.ReleaseOwnedScene(); physics::World world; manager.SetSimulating(false); manager.Update(0, world);
    manager.SetScene(nullptr);
}

TEST_F(ManagedServicesBridgeTest, OwnerUiCannotWritePendingTextButGlobalPointerReadsRemainAvailable)
{
    auto& object = m_scene.CreateGameObject();
    object.AddComponent<scene::UIText>().text = "before";
    bool exists = false;
    ASSERT_EQ(scene::managed_detail::ManagedObjectControls::Component(object, 11, scene::ManagedBridgeOp::COMPONENT_REMOVE, exists), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_UI, 19, false, 0, {}, {"after"}), scene::ManagedBridgeStatus::NOT_FOUND);
    ASSERT_NE(object.GetComponent<scene::UIText>(), nullptr);
    EXPECT_EQ(object.GetComponent<scene::UIText>()->text, "before");
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_UI, 9, true), scene::ManagedBridgeStatus::OK);
}

TEST_F(ManagedServicesBridgeTest, ColliderChangesInvalidateBorrowedInertiaAndRejectOverflow)
{
    auto& object = m_scene.CreateGameObject("Shape"); auto& sphere = object.AddComponent<scene::SphereColliderComponent>();
    auto& body = *Body(object).rigidBody; body.SetInertiaFromCollider(sphere.collider.get());
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::COLLIDER_WRITE, 3, false, 3, {2}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(sphere.radius, 2); EXPECT_EQ(body.GetInertiaCollider(), nullptr);
    const auto* before = sphere.collider.get();
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::COLLIDER_WRITE, 3, false, 3, {std::numeric_limits<float>::max()}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(sphere.collider.get(), before); EXPECT_EQ(sphere.radius, 2);
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::COLLIDER_WRITE, 3, false, 3, {1}, {}, nullptr, false), scene::ManagedBridgeStatus::INVALID_SCOPE);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::COLLIDER_COMMAND, 0, false, 3, {}, {"Ice"}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_result.integer, 1); EXPECT_EQ(sphere.material.dynamicFriction, physics::PhysicsMaterial::Ice.dynamicFriction);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::COLLIDER_WRITE, 11, false, 3, {4}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(sphere.material.restitutionCombine, physics::PhysicsMaterialCombine::Maximum);
    auto& mesh = object.AddComponent<scene::MeshColliderComponent>();
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::COLLIDER_COMMAND, 1, false, 17, {}, {"Assets/Meshes/Body.fbx", "2147483647"}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(mesh.meshIndex, std::numeric_limits<int>::max());
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::COLLIDER_COMMAND, 1, false, 17, {}, {"other", "2147483648"}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(mesh.meshPath, "Assets/Meshes/Body.fbx");
}

TEST_F(ManagedServicesBridgeTest, FilteredQueriesRespectMaskTriggersIgnoreAndCurrentOwnership)
{
    physics::World world;
    auto& first = m_scene.CreateGameObject("First"); first.layer = 1;
    auto& second = m_scene.CreateGameObject("Second"); second.layer = 2;
    auto& a = first.AddComponent<scene::SphereColliderComponent>(); auto& b = second.AddComponent<scene::SphereColliderComponent>(); b.isTrigger = true;
    a.collider->Update({0, 0, 2}, math::Quaternion::Identity()); b.collider->Update({0, 0, 4}, math::Quaternion::Identity());
    world.BeginSceneSync();
    world.SyncCollider({}, {a.collider.get(), nullptr, &a.material, {}, false, 1});
    world.SyncCollider({}, {b.collider.get(), nullptr, &b.material, {}, true, 2});
    world.EndSceneSync();
    std::string input; Append(input, 0.0f); Append(input, 0.0f); Append(input, 1.0f);
    Append(input, uint32_t{4}); Append(input, uint32_t{0}); Append(input, uint64_t{0});
    scene::ManagedBridgeRequest request; request.op = scene::ManagedBridgeOp::PHYSICS_RAYCAST_ALL_FILTERED; request.w = 10;
    request.input = input.data(); request.inputBytes = static_cast<uint32_t>(input.size());
    std::vector<scene::managed_detail::ManagedPhysicsHit> hits;
    ASSERT_EQ(m_physics.Query(m_scene, world, request, hits), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(hits.size(), 1u); EXPECT_EQ(hits[0].object, &second);
    uint32_t mask = UINT32_MAX, triggers = 1; std::memcpy(input.data() + 12, &mask, 4); std::memcpy(input.data() + 16, &triggers, 4);
    ASSERT_EQ(m_physics.Query(m_scene, world, request, hits), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(hits.size(), 1u); EXPECT_EQ(hits[0].object, &first);
    triggers = 0; std::memcpy(input.data() + 16, &triggers, 4);
    ASSERT_EQ(m_physics.Query(m_scene, world, request, hits, &first), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(hits.size(), 1u); EXPECT_EQ(hits[0].object, &second);
    second.RemoveComponent<scene::SphereColliderComponent>();
    ASSERT_EQ(m_physics.Query(m_scene, world, request, hits, &first), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(hits.empty());
}

TEST_F(ManagedServicesBridgeTest, ExtendedRigidBodyControlsMatchNativePointImpulseAndParentRotation)
{
    auto& parent = m_scene.CreateGameObject("Parent"); parent.transform.rotation = math::Quaternion::FromAxisAngle(math::Vector3::UP, 0.4f);
    auto& object = m_scene.CreateGameObject("Body"); object.SetParent(parent);
    auto& component = Body(object); auto& body = *component.rigidBody;
    scene::FlushWorldTransforms(m_scene);
    const math::Quaternion expected = math::Quaternion::FromAxisAngle(math::Vector3::RIGHT, 0.6f);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::PHYSICS_BODY_WRITE_ROTATION, 0, false, 0,
        {expected.x, expected.y, expected.z, expected.w}), scene::ManagedBridgeStatus::OK);
    EXPECT_NEAR(body.GetRotation().x, expected.x, testkit::kTolerance);
    EXPECT_NEAR(object.transform.worldRotation.x, expected.x, testkit::kTolerance);
    scene::ManagedBridgeRequest request; scene::ManagedBridgeReply reply;
    request.op = scene::ManagedBridgeOp::PHYSICS_BODY_WRITE;
    request.index = static_cast<uint32_t>(scene::ManagedPhysicsBodyValue::LINEAR_DRAG); request.x = 0.3f;
    ASSERT_EQ(m_physics.RunBody(object, request, reply, true), scene::ManagedBridgeStatus::OK); EXPECT_NEAR(body.m_linearDrag, 0.3f, testkit::kTolerance);
    request.index = static_cast<uint32_t>(scene::ManagedPhysicsBodyValue::USE_CCD); request.x = 1;
    ASSERT_EQ(m_physics.RunBody(object, request, reply, true), scene::ManagedBridgeStatus::OK); EXPECT_TRUE(body.m_useCCD);
    physics::RigidBody native = body; native.ApplyImpulseAtPoint({1, 0, 0}, {0, 1, 0});
    const std::array<float, 3> point{0, 1, 0}; request.op = scene::ManagedBridgeOp::PHYSICS_BODY_COMMAND;
    request.index = static_cast<uint32_t>(scene::ManagedPhysicsBodyCommand::POINT_IMPULSE); request.x = 1;
    request.input = std::bit_cast<const char*>(point.data()); request.inputBytes = 12;
    ASSERT_EQ(m_physics.RunBody(object, request, reply, true), scene::ManagedBridgeStatus::OK);
    EXPECT_VEC3_NEAR(body.GetVelocity(), native.GetVelocity(), testkit::kTolerance);
    EXPECT_VEC3_NEAR(body.GetAngularVelocity(), native.GetAngularVelocity(), testkit::kTolerance);
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::PHYSICS_BODY_WRITE_ROTATION, 0, false, 0, {0, 0, 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_NEAR(body.GetRotation().x, expected.x, testkit::kTolerance);
}

TEST_F(ManagedServicesBridgeTest, AnimatorAudioAndUiCallsReachNativeComponents)
{
    auto& object = m_scene.CreateGameObject("Actor");
    auto& animator = object.AddComponent<scene::AnimatorComponent>();
    auto& audio = object.AddComponent<scene::AudioSourceComponent>();
    auto& text = object.AddComponent<scene::UIText>();
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_ANIMATOR, 25, false, 0, {2}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(animator.speed, 2);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_AUDIO, 14, false, 0, {2}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(audio.volume, 1);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_AUDIO, 12, false, 0, {}, {"Assets/Audio/Hit.wav"}), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(audio.m_pendingPlay); EXPECT_EQ(audio.clipPath, "Assets/Audio/Hit.wav");
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_UI, 19, false, 0, {}, {"得点: 10"}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(text.text, "得点: 10");
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_UI, 14, true), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(m_result.text.size(), 1u); EXPECT_EQ(m_result.text[0], "得点: 10");
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_UI, 18, false, 0, {1, 1, 1, 1}), scene::ManagedBridgeStatus::NOT_FOUND);
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_ANIMATOR, 25, false, 0, {std::numeric_limits<float>::infinity()}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(animator.speed, 2);
}

TEST_F(ManagedServicesBridgeTest, NavigationWarpPreservesParentLocalAndRejectsUnrepresentablePose)
{
    auto& parent = m_scene.CreateGameObject("Parent"); parent.transform.position = {10, 0, 0}; parent.transform.scale = {2, 2, 2};
    auto& object = m_scene.CreateGameObject("Agent"); object.SetParent(parent);
    auto& agent = object.AddComponent<scene::NavMeshAgentComponent>();
    scene::FlushWorldTransforms(m_scene);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_NAVIGATION, 0, false, 0, {5, 0, 0}), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_NAVIGATION, 4, false, 0, {14, 0, 0}, {}, nullptr, false), scene::ManagedBridgeStatus::OK);
    EXPECT_VEC3_NEAR(object.transform.position, (math::Vector3{2, 0, 0}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(object.transform.worldPosition, (math::Vector3{14, 0, 0}), testkit::kTolerance); EXPECT_FALSE(agent.hasDestination);
    parent.transform.scale.x = 0; scene::FlushWorldTransforms(m_scene);
    const auto before = object.transform;
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_NAVIGATION, 4, false, 0, {15, 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_VEC3_NEAR(object.transform.position, before.position, testkit::kTolerance);
    EXPECT_VEC3_NEAR(object.transform.worldPosition, before.worldPosition, testkit::kTolerance);
}

TEST_F(ManagedServicesBridgeTest, CharacterMutationsValidateCopiesAndContactCallbackException)
{
    auto& object = m_scene.CreateGameObject("Character"); auto& cc = object.AddComponent<scene::CharacterControllerComponent>();
    auto& body = *Body(object).rigidBody;
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CHARACTER, 2, false, 0, {5}), scene::ManagedBridgeStatus::OK);
    EXPECT_NEAR(body.GetVelocity().y, 5, testkit::kTolerance); EXPECT_FALSE(cc.isGrounded);
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CHARACTER, 3, false, 0, {2, 0, 3}, {}, nullptr, false), scene::ManagedBridgeStatus::INVALID_SCOPE);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CHARACTER, 3, false, 0, {2, 0, 3}), scene::ManagedBridgeStatus::OK);
    EXPECT_VEC3_NEAR(body.GetVelocity(), (math::Vector3{2, 5, 3}), testkit::kTolerance);
    body.SetVelocity({std::numeric_limits<float>::max(), 0, 0});
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CHARACTER, 4, false, 0, {std::numeric_limits<float>::max(), 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(body.GetVelocity().x, std::numeric_limits<float>::max()); EXPECT_TRUE(cc.IsFiniteState());
    body.SetVelocity({}); cc.UseAutomaticGrounding();
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CHARACTER, 16, false, 0, {0, 1, 0}, {}, nullptr, false, true), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CHARACTER, 16, false, 0, {0, 1, 0}, {}, nullptr, false, false), scene::ManagedBridgeStatus::INVALID_SCOPE);
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CHARACTER, 11, false, 0, {std::numeric_limits<float>::max(), 0, 0}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_TRUE(cc.IsFiniteState()); EXPECT_TRUE(body.IsFiniteState());
}

TEST_F(ManagedServicesBridgeTest, CameraDirectXDepthRoundTripsAndDistanceDepthStaysDistinct)
{
    auto& object = m_scene.CreateGameObject("Camera"); object.AddComponent<scene::CameraComponent>();
    scene::ScriptRuntime runtime; runtime.viewportWidth = 800; runtime.viewportHeight = 600;
    scene::ScriptRuntime* previous = scene::ScriptRuntime::GetOverride(); scene::ScriptRuntime::Override(&runtime);
    struct Restore { scene::ScriptRuntime* value; ~Restore() { scene::ScriptRuntime::Override(value); } } restore{previous};
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CAMERA, 7, true, 0, {1, 2, 10}), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(m_result.values.size(), 3u); const auto screen = m_result.values;
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CAMERA, 8, true, 0, {screen[0], screen[1], screen[2]}), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(m_result.values.size(), 3u);
    EXPECT_VEC3_NEAR((math::Vector3{m_result.values[0], m_result.values[1], m_result.values[2]}), (math::Vector3{1, 2, 10}), 0.0002f);
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CAMERA, 20, true, 0, {0.5f, 0.5f, 10}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_result.integer, 1); EXPECT_NEAR(m_result.values[2], 10, testkit::kTolerance);
    EXPECT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CAMERA, 3, false, 0, {1, 1}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    object.transform.worldPosition = {std::numeric_limits<float>::max(), 0, 0};
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CAMERA, 19, true, 0, {0, 0, 10}), scene::ManagedBridgeStatus::OK);
    /// @note TryWorldToViewportPoint は画面外でも有限の投影結果なら成功する。
    EXPECT_EQ(m_result.integer, 1); ASSERT_EQ(m_result.values.size(), 3u);
    object.transform.worldPosition.y = std::numeric_limits<float>::max();
    ASSERT_EQ(Run(object, scene::ManagedBridgeOp::SERVICE_CAMERA, 19, true, 0, {0, 0, 10}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_result.integer, 0); EXPECT_TRUE(m_result.values.empty());
}

TEST_F(ManagedServicesBridgeTest, SynthAndLoopUseRealCallerAndLifetimeOwnedHandles)
{
    auto& owner = m_scene.CreateGameObject("Owner"); m_caller.SetContext(&m_scene, &owner);
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_AUDIO, 29, true, int64_t{1} << 32), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(m_result.values.size(), 19u); EXPECT_TRUE(m_result.values[5] > 0);
    auto& foreign = m_scene.CreateGameObject("Other");
    EXPECT_EQ(Run(foreign, scene::ManagedBridgeOp::SERVICE_AUDIO, 42, false, 0, {0.5f, 1, 1, 2, 22}, {"clip.wav", "Loop", "SE"}), scene::ManagedBridgeStatus::INVALID_SCOPE);
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_AUDIO, 42, false, 0, {0.5f, 1, 1, 2, 22}, {"clip.wav", "Loop", "SE"}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_result.integer, 1); ASSERT_EQ(m_result.text.size(), 1u);
    int64_t handle = 0;
    const auto parsed = std::from_chars(m_result.text[0].data(), m_result.text[0].data() + m_result.text[0].size(), handle);
    ASSERT_TRUE(parsed.ec == std::errc{});
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_AUDIO, 45, true, handle), scene::ManagedBridgeStatus::OK); EXPECT_EQ(m_result.integer, 1);
    m_services.CancelOwner(9, &m_caller);
    EXPECT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_AUDIO, 45, true, handle), scene::ManagedBridgeStatus::NOT_FOUND);
}
TEST_F(ManagedServicesBridgeTest, RandomStreamMatchesSdkVectorsFullRangeAndForcedTailRejection)
{
    util::RandomStream raw(1), range(1);
    const std::array<uint64_t, 6> words{0x47E4CE4B896CDD1Dull, 0xABCFA6A8E079651Dull, 0xB9D10D8FEB731F57ull,
        0x4DB418A0BB1B019Dull, 0x0E6199B04D5AA600ull, 0xC8674BCB42E3AAD9ull};
    const std::array<int, 6> values{158129436, 1618568476, 1802706774, 991625628, -849697281, -1025266984};
    for (size_t index = 0; index < words.size(); ++index) {
        EXPECT_EQ(raw.NextUInt(), words[index]); EXPECT_EQ(range.Range(INT32_MIN, INT32_MAX), values[index]);
    }
    util::RandomStream minimum(0x98D76A164D99A710ull), maximum(0x13184FE17DCE7927ull);
    EXPECT_EQ(minimum.Range(INT32_MIN, INT32_MAX), INT32_MIN); EXPECT_EQ(maximum.Range(INT32_MIN, INT32_MAX), INT32_MAX);
    util::RandomStream rejected(0xA8D395BE4B19CCE8ull);
    EXPECT_EQ(rejected.Range(INT32_MIN, INT32_MAX), 1635061397); EXPECT_EQ(rejected.state, 0x68F06B73AF7E230Eull);
    util::RandomStream zero(0), defaults; defaults.state = 0;
    EXPECT_EQ(zero.NextUInt(), defaults.NextUInt()); EXPECT_NE(defaults.state, 0u);
    const uint64_t previous = range.state;
    EXPECT_EQ(range.Range(INT32_MAX, INT32_MAX), INT32_MAX); EXPECT_FLOAT_EQ(range.Range(2.f, 2.f), 2.f); EXPECT_EQ(range.state, previous);
    util::RandomStream finite(1), adjacent(1);
    EXPECT_TRUE(std::isfinite(finite.Range(-std::numeric_limits<float>::max(), std::numeric_limits<float>::max())));
    EXPECT_FLOAT_EQ(adjacent.Range(1.f, std::nextafter(1.f, 2.f)), 1.f);
    util::RandomStream invalid(1); const uint64_t invalidState = invalid.state;
    EXPECT_EQ(invalid.Range(1, -1), 1);
    EXPECT_FLOAT_EQ(invalid.Range(std::numeric_limits<float>::quiet_NaN(), 1.f), 0.f);
    EXPECT_FALSE(invalid.Chance(1.1f)); EXPECT_EQ(invalid.state, invalidState);
}

TEST_F(ManagedServicesBridgeTest, MaterialNeverFallsBackResizesOrAcceptsUnknownSharedGuidAndExpiresPending)
{
    auto& target = m_scene.CreateGameObject("MaterialOwner");
    auto& material = target.AddComponent<scene::MaterialComponent>(); material.ResizeSlots(2);
    material.RawSlotAt(0).visible = true; material.RawSlotAt(1).visible = false;
    material.RawSlotAt(1).SetMaterialPath("original.mat"); EditMaterialAuthoring(material.RawSlotAt(1), [&](auto& values) { values.paramOverrides["glow"] = {4.f}; });
    const auto op = scene::ManagedBridgeOp::SERVICE_MATERIAL;
    m_result.integer = 73;
    EXPECT_EQ(Run(target, op, 4, false, 2, {1.f}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(m_result.integer, 73); EXPECT_TRUE(material.RawSlotAt(0).visible); EXPECT_EQ(material.SlotCount(), 2u);
    ASSERT_EQ(Run(target, op, 4, false, 1, {1.f}), scene::ManagedBridgeStatus::OK); EXPECT_TRUE(material.RawSlotAt(1).visible);
    ASSERT_EQ(Run(target, op, 24, false, 1, {}, {"guid:11111111111111111111111111111111"}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_result.integer, 0); EXPECT_EQ(material.RawSlotAt(1).GetMaterialPath(), "original.mat"); EXPECT_FLOAT_EQ(material.RawSlotAt(1).Authoring().paramOverrides.at("glow")[0], 4.f);
    /// @note この CPU fixture は AssetManager を持たない。未割当 slot の拒否を検証し、共有 path 変更の検証前に旧 path を戻す。
    material.RawSlotAt(1).SetMaterialPath("");
    ASSERT_EQ(Run(target, op, 7, false, 1, {2.f}, {"glow"}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_result.integer, 0); EXPECT_FLOAT_EQ(material.RawSlotAt(1).Authoring().paramOverrides.at("glow")[0], 4.f);
    material.RawSlotAt(1).SetMaterialPath("original.mat");
    ASSERT_EQ(Run(target, op, 24, false, 1, {}, {""}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_result.integer, 1); EXPECT_TRUE(material.RawSlotAt(1).GetMaterialPath().empty()); EXPECT_TRUE(material.RawSlotAt(1).Authoring().paramOverrides.empty());
    bool exists = false;
    uint64_t birth = 0;
    ASSERT_EQ(scene::managed_detail::ManagedObjectControls::Component(target, 29, scene::ManagedBridgeOp::COMPONENT_READ, exists, &birth), scene::ManagedBridgeStatus::OK);
    ASSERT_TRUE(exists); ASSERT_NE(birth, 0u);
    const uint64_t originalBirth = birth;
    ASSERT_EQ(scene::managed_detail::ManagedObjectControls::Component(target, 29, scene::ManagedBridgeOp::COMPONENT_REMOVE, exists), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(Run(target, op, 1, true), scene::ManagedBridgeStatus::NOT_FOUND);
    scene::managed_detail::ManagedObjectControls::FlushScriptRemovals(m_scene);
    ASSERT_EQ(scene::managed_detail::ManagedObjectControls::Component(target, 29, scene::ManagedBridgeOp::COMPONENT_ADD, exists, &birth), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(exists); EXPECT_NE(birth, originalBirth);
}

TEST_F(ManagedServicesBridgeTest, SaveLoadPreservesDirtyValuesOnCorruptUnreadableFilesAndBorrowedViews)
{
    testkit::TempDir files("managed-store"); ASSERT_TRUE(files.IsValid());
    const std::string file = util::FileSystem::PathToUtf8(files.File("save.toml"));
    util::SaveStore store(file); store.SetInt("score", 5); ASSERT_TRUE(store.Load()); EXPECT_FALSE(store.Has("score")); EXPECT_FALSE(store.IsDirty());
    store.SetInt("score", INT32_MIN); ASSERT_TRUE(store.Save()); EXPECT_FALSE(store.IsDirty());
    store.SetInt("score", 7); ASSERT_TRUE(store.Load()); EXPECT_EQ(store.GetInt("score"), INT32_MIN);
    store.SetInt("score", 99); ASSERT_TRUE(util::FileSystem::WriteText(file, "malformed = ["));
    EXPECT_FALSE(store.Load()); EXPECT_EQ(store.GetInt("score"), 99); EXPECT_TRUE(store.IsDirty());
    store.SetPath(util::FileSystem::PathToUtf8(files.Path()));
    EXPECT_FALSE(store.Load()); EXPECT_EQ(store.GetInt("score"), 99); EXPECT_TRUE(store.IsDirty());
    store.SetString("large", std::string(18 * 1024 * 1024, 'z'));
    std::string_view first = "sentinel", second = "unchanged";
    ASSERT_TRUE(store.TryGetStringView("large", first)); ASSERT_TRUE(store.TryGetStringView("large", second));
    EXPECT_EQ(first.data(), second.data()); EXPECT_EQ(first.size(), 18u * 1024u * 1024u);
    std::string_view missing = "sentinel"; EXPECT_FALSE(store.TryGetStringView("absent", missing)); EXPECT_EQ(missing, "sentinel");
    EXPECT_FALSE(store.TryGetStringView("score", missing)); EXPECT_EQ(missing, "sentinel");
}

TEST_F(ManagedServicesBridgeTest, SaveAndConfigUseActualSeparateApplicationStoresAndRejectEditMutation)
{
    scene::ScriptRuntime runtime; RuntimeServiceScope runtimeScope(runtime);
    auto& target = m_scene.CreateGameObject("StoreOwner");
    auto& app = core::Application::Get();
    util::SaveStore originalSave = std::move(app.GetSaveStore()), originalConfig = std::move(app.GetConfigStore());
    app.GetSaveStore() = util::SaveStore("Saves/test.toml"); app.GetConfigStore() = util::SaveStore("Config/test.toml");
    struct Restore { core::Application& app; util::SaveStore& save; util::SaveStore& config;
        ~Restore() { app.GetSaveStore() = std::move(save); app.GetConfigStore() = std::move(config); } } restore{app, originalSave, originalConfig};
    ASSERT_EQ(Run(target, scene::ManagedBridgeOp::SERVICE_SAVE, 11, false, INT32_MIN, {}, {"score"}), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(Run(target, scene::ManagedBridgeOp::SERVICE_CONFIG, 11, false, INT32_MAX, {}, {"score"}), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(Run(target, scene::ManagedBridgeOp::SERVICE_SAVE, 10, true, 0, {}, {"score"}), scene::ManagedBridgeStatus::OK); EXPECT_EQ(m_result.integer, INT32_MIN);
    ASSERT_EQ(Run(target, scene::ManagedBridgeOp::SERVICE_CONFIG, 10, true, 0, {}, {"score"}), scene::ManagedBridgeStatus::OK); EXPECT_EQ(m_result.integer, INT32_MAX);
    ASSERT_EQ(Run(target, scene::ManagedBridgeOp::SERVICE_SAVE, 5, false), scene::ManagedBridgeStatus::OK); EXPECT_EQ(app.GetConfigStore().GetInt("score"), INT32_MAX);
    app.GetSaveStore().SetString("large", std::string(18 * 1024 * 1024, 'z'));
    ASSERT_EQ(Run(target, scene::ManagedBridgeOp::SERVICE_SAVE, 14, true, 0, {}, {"large"}), scene::ManagedBridgeStatus::OK);
    std::string_view view; ASSERT_TRUE(app.GetSaveStore().TryGetStringView("large", view));
    EXPECT_TRUE(m_result.hasBorrowedText); EXPECT_TRUE(m_result.text.empty()); EXPECT_EQ(m_result.borrowedText.data(), view.data());
    ASSERT_EQ(Run(target, scene::ManagedBridgeOp::SERVICE_SAVE, 14, true, 0, {}, {"missing"}), scene::ManagedBridgeStatus::OK);
    EXPECT_FALSE(m_result.hasBorrowedText); EXPECT_TRUE(m_result.text.empty()); EXPECT_EQ(m_result.integer, 0);
    scene::Script::SetInPlayMode(false);
    EXPECT_EQ(Run(target, scene::ManagedBridgeOp::SERVICE_SAVE, 5, false), scene::ManagedBridgeStatus::INVALID_SCOPE);
    EXPECT_TRUE(app.GetSaveStore().Has("large"));
}
TEST_F(ManagedServicesBridgeTest, StoreReceptionPublishesOnceAndFailedOrExpiredPacketsPreserveCleanValues)
{
    scene::ScriptRuntime runtime; RuntimeServiceScope runtimeScope(runtime);
    auto& target = m_scene.CreateGameObject("ReceptionOwner"); auto& app = core::Application::Get();
    util::SaveStore previous = std::move(app.GetSaveStore());
    testkit::TempDir files("store-reception"); ASSERT_TRUE(files.IsValid());
    app.GetSaveStore() = util::SaveStore(util::FileSystem::PathToUtf8(files.File("save.toml")));
    struct Restore { core::Application& app; util::SaveStore& previous; ~Restore() { app.GetSaveStore() = std::move(previous); } } restore{app, previous};
    auto& store = app.GetSaveStore(); store.SetString("value", "old"); ASSERT_TRUE(store.Save()); ASSERT_FALSE(store.IsDirty());
    const auto op = scene::ManagedBridgeOp::SERVICE_SAVE;
    const std::string value = std::string("A\0", 2) + "\xE3\x81\x82" + std::string(2 * 1024 * 1024, 'z');
    ASSERT_EQ(Run(target, op, 22, false, static_cast<int64_t>(value.size()), {}, {"value"}), scene::ManagedBridgeStatus::OK);
    const uint64_t token = static_cast<uint64_t>(m_result.integer);
    ASSERT_EQ(StorePacket(target, 23, token, 0, std::string_view(value).substr(0, 3)), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(store.GetString("value"), "old"); EXPECT_FALSE(store.IsDirty());
    size_t offset = 3;
    while (offset < value.size()) {
        const size_t count = std::min<size_t>(65512, value.size() - offset);
        ASSERT_EQ(StorePacket(target, 23, token, offset, std::string_view(value).substr(offset, count)), scene::ManagedBridgeStatus::OK);
        offset += count; EXPECT_EQ(m_result.integer, static_cast<int64_t>(offset));
    }
    ASSERT_EQ(StorePacket(target, 24, token), scene::ManagedBridgeStatus::OK); EXPECT_EQ(store.GetString("value"), value); EXPECT_TRUE(store.IsDirty());
    EXPECT_EQ(StorePacket(target, 24, token), scene::ManagedBridgeStatus::NOT_FOUND);
    store.SetString("value", "old"); ASSERT_TRUE(store.Save());
    const auto begin = [&](int64_t bytes) { EXPECT_EQ(Run(target, op, 22, false, bytes, {}, {"value"}), scene::ManagedBridgeStatus::OK); return static_cast<uint64_t>(m_result.integer); };
    const uint64_t wrongOffset = begin(1); EXPECT_EQ(StorePacket(target, 23, wrongOffset, 1, "x"), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(StorePacket(target, 24, wrongOffset), scene::ManagedBridgeStatus::NOT_FOUND);
    const uint64_t invalidUtf8 = begin(2); EXPECT_EQ(StorePacket(target, 23, invalidUtf8, 0, "\xC0\x80"), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    const uint64_t truncated = begin(1); ASSERT_EQ(StorePacket(target, 23, truncated, 0, "\xE3"), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(StorePacket(target, 24, truncated), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    const uint64_t wrongOwner = begin(1); EXPECT_EQ(StorePacket(target, 25, wrongOwner, 0, {}, 13, 10), scene::ManagedBridgeStatus::INVALID_SCOPE);
    EXPECT_EQ(StorePacket(target, 25, wrongOwner), scene::ManagedBridgeStatus::OK);
    const uint64_t expired = begin(1); m_services.EndCall(13); EXPECT_EQ(StorePacket(target, 24, expired), scene::ManagedBridgeStatus::NOT_FOUND);
    const uint64_t changedPath = begin(0); store.SetPath(util::FileSystem::PathToUtf8(files.File("other.toml")));
    EXPECT_EQ(StorePacket(target, 24, changedPath), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(store.GetString("value"), "old"); EXPECT_FALSE(store.IsDirty());
}
TEST_F(ManagedServicesBridgeTest, PortableDocumentsRejectMalformedCandidatesAndPreserveOldValueRevisionAndDirty)
{
    testkit::TempDir files("portable-store"); ASSERT_TRUE(files.IsValid());
    util::SaveStore store(util::FileSystem::PathToUtf8(files.File("save.toml")));
    const std::string type = "Game:Profile", schema(64, 'a');
    const auto envelope = [&](std::string_view value) { return "{\"formatVersion\":1,\"type\":\"" + type + "\",\"schema\":\"" + schema + "\",\"value\":" + std::string(value) + "}"; };
    const std::string original = envelope("{\"score\":7,\"text\":\"あ\\u0000tail\"}");
    ASSERT_TRUE(store.CommitDocument("profile", type, schema, std::string(original), store.GetRevision(), store.GetPath()));
    ASSERT_TRUE(store.Save()); ASSERT_FALSE(store.IsDirty());
    const uint64_t revision = store.GetRevision();
    const std::array<std::string, 9> invalid{
        envelope("{\"score\":1,\"score\":2}"), envelope("1e999"), envelope("[1,]"), envelope("\"\\uD800\""),
        envelope("\"" + std::string("\xC0\x80", 2) + "\""),
        "{\"formatVersion\":2,\"type\":\"" + type + "\",\"schema\":\"" + schema + "\",\"value\":0}",
        "{\"formatVersion\":1,\"type\":\"Wrong\",\"schema\":\"" + schema + "\",\"value\":0}",
        "{\"formatVersion\":1,\"type\":\"" + type + "\",\"schema\":\"different\",\"value\":0}",
        original.substr(0, original.size() - 1)};
    for (const auto& candidate : invalid) {
        EXPECT_FALSE(store.CommitDocument("profile", type, schema, std::string(candidate), revision, store.GetPath()));
        EXPECT_FALSE(store.IsDirty()); EXPECT_EQ(store.GetRevision(), revision);
        std::string_view storedType, storedSchema, storedJson;
        ASSERT_TRUE(store.TryGetDocumentView("profile", storedType, storedSchema, storedJson)); EXPECT_EQ(storedJson, original);
    }
    std::string large = "[0"; for (unsigned index = 1; index < 40001; ++index) large += ",0"; large += "]";
    ASSERT_TRUE(store.CommitDocument("profile", type, schema, envelope(large), revision, store.GetPath()));
    ASSERT_TRUE(store.Save()); ASSERT_TRUE(store.Load());
    std::string_view storedType, storedSchema, storedJson; ASSERT_TRUE(store.TryGetDocumentView("profile", storedType, storedSchema, storedJson));
    EXPECT_EQ(storedJson, envelope(large));
    store.SetInt("other", 9); const uint64_t dirtyRevision = store.GetRevision();
    EXPECT_FALSE(store.CommitDocument("profile", type, schema, envelope("[1,]"), dirtyRevision, store.GetPath()));
    EXPECT_TRUE(store.IsDirty()); EXPECT_EQ(store.GetRevision(), dirtyRevision);
    EXPECT_FALSE(store.CommitDocument("profile", type, schema, envelope("0"), dirtyRevision - 1, store.GetPath()));
    EXPECT_FALSE(store.CommitDocument("profile", type, schema, envelope("0"), dirtyRevision, "wrong.toml"));
}

TEST_F(ManagedServicesBridgeTest, MaterialRenderOverridesUseTheExplicitSlotAndRejectInvalidStateWithoutMutation)
{
    auto& target = m_scene.CreateGameObject("RenderOwner"); auto& component = target.AddComponent<scene::MaterialComponent>(); component.ResizeSlots(2);
    const auto op = scene::ManagedBridgeOp::SERVICE_MATERIAL;
    ASSERT_EQ(Run(target, op, 36, false, 1, {3}), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(component.RawSlotAt(1).Authoring().hasBlendModeOverride); EXPECT_FALSE(component.RawSlotAt(0).Authoring().hasBlendModeOverride);
    ASSERT_EQ(Run(target, op, 37, true, 1), scene::ManagedBridgeStatus::OK); EXPECT_EQ(m_result.integer, 3);
    EXPECT_EQ(Run(target, op, 36, false, 1, {3.5f}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(component.RawSlotAt(1).Authoring().blendModeOverride, renderer::BlendMode::PREMULTIPLIED);
    ASSERT_EQ(Run(target, op, 38, false, 1, {1}), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(Run(target, op, 39, true, 1), scene::ManagedBridgeStatus::OK); EXPECT_EQ(m_result.integer, 1);
    ASSERT_EQ(Run(target, op, 40, false, 1, {}, {"-2147483648"}), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(Run(target, op, 41, true, 1), scene::ManagedBridgeStatus::OK); EXPECT_EQ(m_result.integer, INT32_MIN);
    EXPECT_EQ(Run(target, op, 40, false, 1, {}, {"2147483648"}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(component.RawSlotAt(1).Authoring().renderQueueOverride, INT32_MIN);
    EXPECT_EQ(Run(target, op, 36, true, 1), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    ASSERT_EQ(Run(target, op, 23, false, 1), scene::ManagedBridgeStatus::OK);
    EXPECT_FALSE(component.RawSlotAt(1).Authoring().hasBlendModeOverride); EXPECT_FALSE(component.RawSlotAt(1).Authoring().hasDoubleSidedOverride); EXPECT_FALSE(component.RawSlotAt(1).Authoring().hasRenderQueueOverride);
}
TEST_F(ManagedServicesBridgeTest, RenderUsesOnlyTheRealPlayCopyAndGameViewportWithAtomicValidation)
{
    auto& owner = m_scene.CreateGameObject("RenderOwner"); m_caller.SetContext(&m_scene, &owner);
    auto& app = core::Application::Get(); auto* previous = app.GetActiveRenderSettings();
    renderer::RenderSettings edit; edit.userBrightness = 0.8f; renderer::RenderSettings play = edit;
    struct Restore { core::Application& app; renderer::RenderSettings* previous; ~Restore() { app.SetActiveRenderSettings(previous); } } restore{app, previous};
    scene::ScriptRuntime runtime; runtime.viewportWidth = 800; runtime.viewportHeight = 600; RuntimeServiceScope scope(runtime);
    app.SetActiveRenderSettings(nullptr);
    EXPECT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_RENDER, 1, true), scene::ManagedBridgeStatus::INVALID_SCOPE);
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_RENDER, 0, true), scene::ManagedBridgeStatus::OK); EXPECT_EQ(m_result.integer, 0);
    app.SetActiveRenderSettings(&play);
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_RENDER, 2, false, 0, {10}), scene::ManagedBridgeStatus::OK);
    EXPECT_FLOAT_EQ(play.userBrightness, 4); EXPECT_FLOAT_EQ(edit.userBrightness, 0.8f);
    EXPECT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_RENDER, 2, false, 0, {std::numeric_limits<float>::quiet_NaN()}), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_FLOAT_EQ(play.userBrightness, 4);
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_RENDER, 6, false, 0, {0.5f}), scene::ManagedBridgeStatus::OK);
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_RENDER, 7, true), scene::ManagedBridgeStatus::OK); EXPECT_EQ(m_result.integer, 640);
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_RENDER, 8, true), scene::ManagedBridgeStatus::OK); EXPECT_EQ(m_result.integer, 360);
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_RENDER, 20, false, 0, {1}), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(play.gtao.enabled); EXPECT_FALSE(play.postProcess.ambientOcclusion.enabled);
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_RENDER, 22, false, 0, {1}), scene::ManagedBridgeStatus::OK);
    EXPECT_TRUE(play.postProcess.ambientOcclusion.enabled); EXPECT_FALSE(play.gtao.enabled);
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_RENDER, 16, false, INT32_MAX), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(play.shadow.cascadeCount, 4);
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_RENDER, 16, false, INT32_MIN), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(play.shadow.cascadeCount, 1);
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_RENDER, 14, false, INT32_MAX), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(play.shadow.mapResolution, 8192U);
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_RENDER, 14, false, 1), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(play.shadow.mapResolution, 512U);
    auto& foreign = m_scene.CreateGameObject("Foreign");
    EXPECT_EQ(Run(foreign, scene::ManagedBridgeOp::SERVICE_RENDER, 2, false, 0, {1}), scene::ManagedBridgeStatus::INVALID_SCOPE);
}

TEST_F(ManagedServicesBridgeTest, JointRequiresFixedUpdateAndPreservesOldSettingsWhenAnyTargetOrAxisIsInvalid)
{
    auto& owner = m_scene.CreateGameObject("JointOwner"); auto& target = m_scene.CreateGameObject("Body");
    Body(owner); Body(target); auto& joint = owner.AddComponent<scene::JointComponent>(); joint.distance = 7;
    const auto op = scene::ManagedBridgeOp::SERVICE_JOINT;
    EXPECT_EQ(Run(owner, op, 8, false, 0, {3}, {}, &target, false), scene::ManagedBridgeStatus::INVALID_SCOPE); EXPECT_EQ(joint.distance, 7);
    ASSERT_EQ(Run(owner, op, 8, false, 0, {3}, {}, &target, true), scene::ManagedBridgeStatus::OK); EXPECT_EQ(m_result.integer, 1); EXPECT_EQ(joint.distance, 3);
    const auto body = joint.connectedBody.id;
    EXPECT_EQ(Run(owner, op, 11, false, 0, {0, 0, 0, 0, 0, 0, 0, 0, 0}, {}, &target, true), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
    EXPECT_EQ(joint.connectedBody.id, body); EXPECT_EQ(joint.type, scene::JointType::Distance);
    ASSERT_EQ(Run(owner, op, 7, false, 0, {}, {}, &owner, true), scene::ManagedBridgeStatus::OK); EXPECT_EQ(m_result.integer, 0); EXPECT_EQ(joint.connectedBody.id, body);
    ASSERT_EQ(Run(owner, op, 14, false, 0, {80, -20}), scene::ManagedBridgeStatus::OK); EXPECT_EQ(joint.lowerLimit, -20); EXPECT_EQ(joint.upperLimit, 80);
    ASSERT_EQ(Run(owner, op, 13, false), scene::ManagedBridgeStatus::OK); EXPECT_FALSE(joint.enabled);
    EXPECT_EQ(Run(owner, op, 0, true), scene::ManagedBridgeStatus::NOT_FOUND);
    bool exists = false; EXPECT_EQ(scene::managed_detail::ManagedObjectControls::Component(owner, static_cast<uint32_t>(scene::ManagedNativeComponentKind::JOINT), scene::ManagedBridgeOp::COMPONENT_ADD, exists), scene::ManagedBridgeStatus::INVALID_ARGUMENT);
}

TEST_F(ManagedServicesBridgeTest, DisplayUsesHostIntentAndRejectsInvalidDimensionsWithoutRequesting)
{
    auto& owner = m_scene.CreateGameObject("DisplayOwner"); m_caller.SetContext(&m_scene, &owner);
    struct State { int calls = 0; uint64_t lifetime = 0; uint32_t operation = 0, first = 0, second = 0; } state;
    scene::ScriptRuntime runtime; runtime.displayContext = &state;
    runtime.requestDisplay = [](void* context, uint64_t lifetime, uint32_t operation, uint32_t first, uint32_t second) {
        auto& value = *static_cast<State*>(context); ++value.calls; value.lifetime = lifetime; value.operation = operation; value.first = first; value.second = second; return true;
    };
    RuntimeServiceScope scope(runtime);
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_DISPLAY, 7, false, 1280, {}, {"720"}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_result.integer, 1); EXPECT_EQ(state.calls, 1); EXPECT_EQ(state.lifetime, m_scene.GetRenderSceneGeneration()); EXPECT_EQ(state.operation, 1u); EXPECT_EQ(state.second, 720u);
    EXPECT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_DISPLAY, 7, false, 0, {}, {"720"}), scene::ManagedBridgeStatus::INVALID_ARGUMENT); EXPECT_EQ(state.calls, 1);
    EXPECT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_DISPLAY, 8, false, 2), scene::ManagedBridgeStatus::INVALID_ARGUMENT); EXPECT_EQ(state.calls, 1);
    scene::Script::SetInPlayMode(false);
    EXPECT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_DISPLAY, 6, false, 1), scene::ManagedBridgeStatus::INVALID_SCOPE); EXPECT_EQ(state.calls, 1);
}
TEST_F(ManagedServicesBridgeTest, JointChainKeepsTheOldRowUntilEveryTargetAndBirthFencePasses)
{
    auto& owner = m_scene.CreateGameObject("Chain"); auto& target = m_scene.CreateGameObject("Link");
    Body(owner); Body(target); auto& joint = owner.AddComponent<scene::JointComponent>(); joint.distance = 7;
    struct Context { scene::GameObject* first; } context{&target};
    const auto resolve = [](void* state, uint64_t token, scene::GameObject*& object) {
        object = token == 1 ? static_cast<Context*>(state)->first : nullptr; return scene::ManagedBridgeStatus::OK;
    };
    const auto packet = [&](uint32_t index, uint64_t token, std::initializer_list<uint64_t> targets) {
        std::string bytes; Append(bytes, token);
        if (index == 20) { Append(bytes, uint64_t{0}); Append(bytes, static_cast<uint32_t>(targets.size())); Append(bytes, uint32_t{0});
            for (uint64_t id : targets) { Append(bytes, id); Append(bytes, m_scene.GetRenderSceneGeneration()); } }
        scene::ManagedBridgeRequest request; request.op = scene::ManagedBridgeOp::SERVICE_JOINT; request.index = index;
        request.ownerId = 9; request.generation = 7; request.instanceId = 11; request.callId = 13; request.input = bytes.data(); request.inputBytes = static_cast<uint32_t>(bytes.size());
        return m_services.RunJointPacket(m_scene, owner, request, m_result, resolve, &context, true);
    };
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_JOINT, 19, false, 2, {1}, {"4"}), scene::ManagedBridgeStatus::OK); uint64_t token = static_cast<uint64_t>(m_result.integer);
    ASSERT_EQ(packet(20, token, {1, 2}), scene::ManagedBridgeStatus::OK); EXPECT_EQ(joint.distance, 7);
    ASSERT_EQ(packet(22, token, {}), scene::ManagedBridgeStatus::OK); EXPECT_EQ(m_result.integer, 0); EXPECT_EQ(joint.distance, 7); EXPECT_TRUE(joint.chainBodies.empty());
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_JOINT, 19, false, 1, {2}, {"4"}), scene::ManagedBridgeStatus::OK); token = static_cast<uint64_t>(m_result.integer);
    ASSERT_EQ(packet(20, token, {1}), scene::ManagedBridgeStatus::OK); ASSERT_EQ(packet(22, token, {}), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(m_result.integer, 1); ASSERT_EQ(joint.chainBodies.size(), 1u); EXPECT_EQ(joint.chainBodies[0].id, target.GetID()); EXPECT_EQ(joint.distance, 2);
    const std::string saved = scene::SceneSerializer::SaveToText(m_scene);
    ASSERT_FALSE(saved.empty());
    const auto restored = scene::SceneSerializer::LoadDataFromText(saved, ""); ASSERT_NE(restored, nullptr);
    scene::GameObject* restoredOwner = nullptr;
    for (scene::GameObject& object : restored->GameObjects()) if (object.name == owner.name) restoredOwner = &object;
    ASSERT_NE(restoredOwner, nullptr);
    const auto* restoredJoint = restoredOwner->GetComponent<scene::JointComponent>(); ASSERT_NE(restoredJoint, nullptr);
    ASSERT_EQ(restoredJoint->chainBodies.size(), 1u);
    scene::GameObject* restoredTarget = restoredJoint->chainBodies.front().Resolve(*restored); ASSERT_NE(restoredTarget, nullptr);
    EXPECT_EQ(restoredTarget->name, target.name); EXPECT_FLOAT_EQ(restoredJoint->distance, 2);
    ASSERT_EQ(Run(owner, scene::ManagedBridgeOp::SERVICE_JOINT, 19, false, 1, {4}, {"4"}), scene::ManagedBridgeStatus::OK); token = static_cast<uint64_t>(m_result.integer);
    ASSERT_EQ(packet(20, token, {1}), scene::ManagedBridgeStatus::OK);
    bool exists = false; ASSERT_EQ(scene::managed_detail::ManagedObjectControls::Component(owner, static_cast<uint32_t>(scene::ManagedNativeComponentKind::JOINT), scene::ManagedBridgeOp::COMPONENT_REMOVE, exists), scene::ManagedBridgeStatus::OK);
    EXPECT_EQ(packet(22, token, {}), scene::ManagedBridgeStatus::NOT_FOUND); EXPECT_EQ(joint.distance, 2);
}
TEST_F(ManagedServicesBridgeTest, SceneTerrainPoolMatchesFlatOrderTiesAfterReorderRemovalAndExchange)
{
    auto& observer=m_scene.CreateGameObject("Observer");m_caller.SetContext(&m_scene,&observer);
    for (int i=0;i<128;++i)m_scene.CreateGameObject("Bone decoration");
    auto& first=m_scene.CreateGameObject("First terrain");auto& firstTerrain=first.AddComponent<scene::TerrainComponent>();SetQueryPlanes(firstTerrain,0);
    auto& second=m_scene.CreateGameObject("Second terrain");auto& secondTerrain=second.AddComponent<scene::TerrainComponent>();SetQueryPlanes(secondTerrain,.4f);
    ExpectSceneTerrainEquivalent(m_scene,m_caller,{0,-4,.5f});
    ASSERT_TRUE(m_scene.MoveGameObjectToIndex(second.GetID(),0));
    ExpectSceneTerrainEquivalent(m_scene,m_caller,{0,-4,.5f});
    first.RemoveComponent<scene::TerrainComponent>();
    auto& third=m_scene.CreateGameObject("Readded terrain");auto& thirdTerrain=third.AddComponent<scene::TerrainComponent>();SetQueryPlanes(thirdTerrain,-.4f);
    ExpectSceneTerrainEquivalent(m_scene,m_caller,{0,-4,.5f});
    second.SetActive(false);ExpectSceneTerrainEquivalent(m_scene,m_caller,{.5f,-4,.5f});
    thirdTerrain.enabled=false;ExpectSceneTerrainEquivalent(m_scene,m_caller,{.5f,-4,.5f});
    m_caller.SetContext(nullptr,nullptr);
    math::Vector3 position{77,88,99},normal{1,2,3};
    EXPECT_FALSE(m_caller.scene.TryGetTerrainFloorAt({0,-4,.5f},10,position,normal));EXPECT_VEC3_NEAR(position,math::Vector3(77,88,99),0);
    scene::Scene other;auto& otherObserver=other.CreateGameObject("New observer");m_caller.SetContext(&other,&otherObserver);
    ExpectSceneTerrainEquivalent(other,m_caller,{0,-4,.5f});m_caller.SetContext(nullptr,nullptr);
}
TEST_F(ManagedServicesBridgeTest, SceneTerrainPoolPreservesWorldPoseReferenceYHolesAndBoundaryTriangles)
{
    auto& observer=m_scene.CreateGameObject("Observer");m_caller.SetContext(&m_scene,&observer);
    auto& cave=m_scene.CreateGameObject("Cave");auto& terrain=cave.AddComponent<scene::TerrainComponent>();SetQueryPlanes(terrain,.2f);
    std::array<scene::Transform,3> poses{};poses[1].worldPosition={100,20,-50};poses[1].worldScale={2,3,.5f};
    poses[2].worldScale={-2,3,4};poses[2].worldRotation=math::Quaternion::FromEuler({.2f,.4f,.1f});
    for (const auto& pose:poses) {
        cave.transform=pose;
        for (float x:std::array<float,3>{0,.5f,2}) for (float y:std::array<float,3>{-6,-4,-2}) {
            const auto reference=pose.worldPosition+pose.worldRotation*math::Vector3{x*pose.worldScale.x,y*pose.worldScale.y,.5f*pose.worldScale.z};
            ExpectSceneTerrainEquivalent(m_scene,m_caller,reference);
        }
    }
    m_caller.SetContext(nullptr,nullptr);
}
} /// @note namespace fbzz::tests
