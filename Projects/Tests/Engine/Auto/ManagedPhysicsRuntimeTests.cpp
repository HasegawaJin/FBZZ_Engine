/// @file    ManagedPhysicsRuntimeTests.cpp
/// @brief   実 CoreCLR から剛体・query・位相拒否・保持 facade の native 接続を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Physics/World.hpp>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::tests {

class ManagedPhysicsRuntimeTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    physics::World m_world;
    physics::World* m_previousWorld = nullptr;
    bool m_previousPlay = false;
    scene::GameObject* m_owner = nullptr;
    scene::ManagedScriptAdapter* m_adapter = nullptr;

    void SetUp() override
    {
        EngineFixture::SetUp();
        m_previousWorld = scene::Script::GetPhysicsWorld();
        m_previousPlay = scene::Script::IsInPlayMode();
        scene::Script::SetInPlayMode(true);
        scene::Script::SetPhysicsWorld(&m_world);
        m_world.SetGravity({0, -2, 0});
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
        m_owner = &m_scene.CreateGameObject("PhysicsCaller");
        m_adapter = Attach(*m_owner);
        ASSERT_NE(m_adapter, nullptr);
    }

    void TearDown() override
    {
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        runtime.Unload(&m_scene); m_scene.Clear();
        m_owner = nullptr; m_adapter = nullptr;
        EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic();
        scene::Script::SetPhysicsWorld(m_previousWorld);
        scene::Script::SetInPlayMode(m_previousPlay);
        EngineFixture::TearDown();
    }

    scene::ManagedScriptAdapter* Attach(scene::GameObject& object)
    {
        auto* component = object.GetComponent<scene::ScriptComponent>();
        if (!component) component = &object.AddComponent<scene::ScriptComponent>();
        auto& entry = component->scripts.emplace_back();
    entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.PhysicsComponent", entry.savedScriptId);
        if (!entry.script) return nullptr;
        entry.script->SetContext(&m_scene, &object);
        auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
        return adapter && adapter->RefreshOwnerBinding() ? adapter : nullptr;
    }
    physics::RigidBody& Body(scene::GameObject& object)
    {
        auto& component = object.AddComponent<scene::RigidBodyComponent>();
        component.rigidBody = std::make_unique<physics::RigidBody>();
        component.rigidBody->m_useGravity = false;
        return *component.rigidBody;
    }
    bool Call(scene::ManagedScriptAdapter& adapter, scene::ScriptCallbackKind kind)
    {
        if (kind == scene::ScriptCallbackKind::AWAKE)
            return adapter.ExecuteProfiledCallback(&scene::Script::OnAwake, kind, "OnAwake");
        if (kind == scene::ScriptCallbackKind::UPDATE)
            return adapter.ExecuteProfiledCallback(&scene::Script::OnUpdate, kind, "OnUpdate");
        return adapter.ExecuteProfiledCallback(&scene::Script::OnFixedUpdate, kind, "OnFixedUpdate");
    }
    bool Fixed() { return Call(*m_adapter, scene::ScriptCallbackKind::FIXED_UPDATE); }
    bool Configure(std::string_view json) { return m_adapter->ApplySettingsJson(json); }
    scene::ManagedBridgeReply Field(std::string_view field, std::string* bytes = nullptr)
    {
        scene::ManagedBridgeRequest request;
        request.op = scene::ManagedBridgeOp::READ_FIELD;
        request.generation = m_adapter->Generation(); request.instanceId = m_adapter->InstanceId();
        request.ownerId = m_adapter->OwnerId(); request.input = field.data(); request.inputBytes = static_cast<uint32_t>(field.size());
        scene::ManagedBridgeReply reply;
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        EXPECT_TRUE(runtime.Dispatch(request, reply, bytes)) << field << ": " << runtime.LastDiagnostic();
        return reply;
    }
    math::Vector3 Vector(std::string_view field)
    { const auto result = Field(field); return {result.x, result.y, result.z}; }
    bool Flag(std::string_view field) { return Field(field).integer != 0; }
    template<typename T>
    T Read(const std::string& bytes, size_t offset)
    {
        T value{};
        if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) { ADD_FAILURE() << "Truncated field payload"; return value; }
        std::memcpy(&value, bytes.data() + offset, sizeof(T)); return value;
    }
    std::vector<std::string> ReferenceArray(std::string_view field)
    {
        std::string bytes; EXPECT_EQ(Field(field, &bytes).valueKind, scene::ManagedValueKind::GAME_OBJECT_ARRAY);
        const uint32_t count = Read<uint32_t>(bytes, 0); size_t offset = 4;
        std::vector<std::string> values;
        for (uint32_t index = 0; index < count; ++index) {
            const int32_t length = Read<int32_t>(bytes, offset); offset += 4;
            if (length < 0 || offset > bytes.size() || static_cast<size_t>(length) > bytes.size() - offset) {
                ADD_FAILURE() << "Invalid query reference payload"; return {};
            }
            values.emplace_back(bytes.substr(offset, static_cast<size_t>(length))); offset += static_cast<size_t>(length);
        }
        EXPECT_EQ(offset, bytes.size()); return values;
    }
    void Sync(scene::ColliderComponent& component, const math::Vector3& position)
    {
        component.collider->Update(position, math::Quaternion::Identity());
        m_world.SyncCollider({}, {component.collider.get(), nullptr, &component.material, math::Vector3::ZERO, false, 0});
    }
};

TEST_F(ManagedPhysicsRuntimeTest, FixedCallbacksDriveNativeBodyAndPreserveParentAndAxisContracts)
{
    auto& parent = m_scene.CreateGameObject("Parent");
    auto& target = m_scene.CreateGameObject("Target");
    auto& child = m_scene.CreateGameObject("Child");
    ASSERT_TRUE(target.SetParent(&parent)); ASSERT_TRUE(child.SetParent(&target));
    parent.transform.position = {10, 0, 0}; parent.transform.scale = {2, 2, 2};
    child.transform.position = {1, 0, 0}; scene::FlushWorldTransforms(m_scene);
    auto& body = Body(target); body.SetPosition(target.transform.worldPosition);
    auto& initialShape = target.AddComponent<scene::SphereColliderComponent>();
    body.SetInertiaFromCollider(initialShape.collider.get());
    ASSERT_TRUE(Configure("{\"target\":\"" + target.instanceId + "\",\"useCached\":true}"));
    ASSERT_TRUE(Call(*m_adapter, scene::ScriptCallbackKind::AWAKE));
    ASSERT_TRUE(Configure(R"({"operation":3,"scalar":2})")); ASSERT_TRUE(Fixed());
    EXPECT_FLOAT_EQ(body.GetMass(), 2);
    ASSERT_TRUE(Configure(R"({"operation":1,"value":[1,2,3]})")); ASSERT_TRUE(Fixed());
    EXPECT_VEC3_NEAR(body.GetVelocity(), (math::Vector3{1, 2, 3}), testkit::kTolerance);
    ASSERT_TRUE(Configure(R"({"operation":7,"value":[4,0,0]})")); ASSERT_TRUE(Fixed());
    EXPECT_VEC3_NEAR(body.GetVelocity(), (math::Vector3{3, 2, 3}), testkit::kTolerance);
    ASSERT_TRUE(Configure(R"({"operation":12,"freezeY":true})")); ASSERT_TRUE(Fixed());
    EXPECT_VEC3_NEAR(body.GetVelocity(), (math::Vector3{3, 0, 3}), testkit::kTolerance);
    ASSERT_TRUE(Configure(R"({"operation":14,"value":[20,40,60]})")); ASSERT_TRUE(Fixed());
    EXPECT_VEC3_NEAR(body.GetPosition(), (math::Vector3{20, 0, 60}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(Vector("bodyPosition"), body.GetPosition(), testkit::kTolerance);
    EXPECT_VEC3_NEAR(target.transform.worldPosition, body.GetPosition(), testkit::kTolerance);
    EXPECT_VEC3_NEAR(child.transform.worldPosition, (math::Vector3{22, 0, 60}), testkit::kTolerance);
    ASSERT_TRUE(Configure(R"({"operation":11})")); ASSERT_TRUE(Fixed()); EXPECT_TRUE(body.IsSleeping());
    ASSERT_TRUE(Configure(R"({"operation":6,"value":[4,0,0]})")); ASSERT_TRUE(Fixed()); EXPECT_FALSE(body.IsSleeping());
    body.Integrate(0.5f);
    EXPECT_VEC3_NEAR(body.GetVelocity(), (math::Vector3{1, 0, 0}), testkit::kTolerance);
    target.RemoveComponent<scene::SphereColliderComponent>();
    ASSERT_TRUE(Configure(R"({"operation":3,"scalar":4})")); ASSERT_TRUE(Fixed());
    EXPECT_EQ(body.GetInertiaCollider(), nullptr); EXPECT_FLOAT_EQ(body.GetMass(), 4);
    auto& replacementShape = target.AddComponent<scene::SphereColliderComponent>();
    body.SetInertiaFromCollider(replacementShape.collider.get());
    const physics::Collider* oldSource = body.GetInertiaCollider();
    replacementShape.SetRadius(1); ASSERT_NE(oldSource, replacementShape.collider.get());
    ASSERT_TRUE(Configure(R"({"operation":5,"flag":true})")); ASSERT_TRUE(Fixed());
    EXPECT_EQ(body.GetInertiaCollider(), nullptr); EXPECT_TRUE(body.IsStatic()); EXPECT_TRUE(body.IsFiniteState());
    EXPECT_VEC3_NEAR(Vector("worldGravity"), (math::Vector3{0, -2, 0}), testkit::kTolerance);
    EXPECT_TRUE(Flag("bodyAlive")); EXPECT_TRUE(Flag("bodyEnabled")); EXPECT_FALSE(m_adapter->IsRuntimeFaulted());
}

TEST_F(ManagedPhysicsRuntimeTest, CastWireReturnsShapeHitsAndReferencesExpireWithoutGuidRevival)
{
    auto& target = m_scene.CreateGameObject("TwoShapes");
    auto& sphere = target.AddComponent<scene::SphereColliderComponent>();
    auto& box = target.AddComponent<scene::BoxColliderComponent>();
    const std::string guid = target.instanceId;
    const auto id = target.GetID();
    m_world.BeginSceneSync(); Sync(sphere, {0, 0, 5}); Sync(box, {0, 0, 5}); m_world.EndSceneSync();
    ASSERT_TRUE(Call(*m_adapter, scene::ScriptCallbackKind::AWAKE));
    ASSERT_TRUE(Configure(R"({"operation":15,"runInUpdate":true,"maxDistance":100})"));
    ASSERT_TRUE(Call(*m_adapter, scene::ScriptCallbackKind::UPDATE));
    EXPECT_TRUE(Flag("hitFound")); EXPECT_TRUE(Flag("hitObjectAlive"));
    std::string hitGuid; EXPECT_NE(Field("hitObject", &hitGuid).integer, 0); EXPECT_EQ(hitGuid, guid);
    EXPECT_NEAR(Field("hitDistance").x, 4.5f, testkit::kTolerance);
    ASSERT_TRUE(Configure(R"({"operation":16,"radius":0.5})"));
    ASSERT_TRUE(Call(*m_adapter, scene::ScriptCallbackKind::UPDATE));
    EXPECT_NEAR(Field("hitDistance").x, 4, testkit::kTolerance);
    EXPECT_VEC3_NEAR(Vector("hitPoint"), (math::Vector3{0, 0, 4}), testkit::kTolerance);
    ASSERT_TRUE(Configure(R"({"operation":17})")); ASSERT_TRUE(Call(*m_adapter, scene::ScriptCallbackKind::UPDATE));
    EXPECT_EQ(ReferenceArray("hits"), (std::vector<std::string>{guid, guid}));
    std::string distances; EXPECT_EQ(Field("distances", &distances).valueKind, scene::ManagedValueKind::FLOAT_ARRAY);
    ASSERT_EQ(Read<uint32_t>(distances, 0), 2u); EXPECT_NEAR(Read<float>(distances, 4), 4.5f, testkit::kTolerance);
    ASSERT_TRUE(Configure(R"({"operation":18,"origin":[0,0,5],"radius":1})"));
    ASSERT_TRUE(Call(*m_adapter, scene::ScriptCallbackKind::UPDATE));
    EXPECT_EQ(ReferenceArray("hits"), (std::vector<std::string>{guid}));
    ASSERT_TRUE(m_scene.DestroyGameObject(id)); m_scene.FlushDestroyQueue(0);
    auto& replacement = m_scene.CreateGameObject("SameGuidReplacement"); replacement.instanceId = guid;
    ASSERT_TRUE(Configure(R"({"operation":0})")); ASSERT_TRUE(Call(*m_adapter, scene::ScriptCallbackKind::UPDATE));
    EXPECT_FALSE(Flag("hitObjectAlive")); EXPECT_EQ(Field("hitObject", &hitGuid).integer != 0, true); EXPECT_EQ(hitGuid, guid);
    ASSERT_TRUE(Configure(R"({"operation":15,"origin":[0,0,0]})")); ASSERT_TRUE(Call(*m_adapter, scene::ScriptCallbackKind::UPDATE));
    EXPECT_FALSE(Flag("hitFound")); EXPECT_FALSE(m_adapter->IsRuntimeFaulted());
}

TEST_F(ManagedPhysicsRuntimeTest, UpdateMutationAndDisabledBodyFaultWithoutPartialNativeEffects)
{
    auto& body = Body(*m_owner);
    body.Sleep();
    ASSERT_TRUE(Call(*m_adapter, scene::ScriptCallbackKind::AWAKE));
    ASSERT_TRUE(Configure(R"({"operation":6,"value":[1,2,3],"runInUpdate":true})"));
    EXPECT_FALSE(Call(*m_adapter, scene::ScriptCallbackKind::UPDATE));
    EXPECT_TRUE(m_adapter->IsRuntimeFaulted()); EXPECT_FALSE(m_adapter->enabled);
    EXPECT_NE(m_adapter->FaultDiagnostic().find("FixedUpdate"), std::string::npos);
    EXPECT_TRUE(body.IsSleeping()); EXPECT_TRUE(body.IsFiniteState());
    auto& other = m_scene.CreateGameObject("DisabledBody");
    auto& disabled = Body(other); disabled.Sleep(); other.GetComponent<scene::RigidBodyComponent>()->enabled = false;
    auto* adapter = Attach(other); ASSERT_NE(adapter, nullptr);
    ASSERT_TRUE(Call(*adapter, scene::ScriptCallbackKind::AWAKE));
    ASSERT_TRUE(adapter->ApplySettingsJson(R"({"operation":7,"value":[4,5,6]})"));
    EXPECT_FALSE(Call(*adapter, scene::ScriptCallbackKind::FIXED_UPDATE)); EXPECT_TRUE(adapter->IsRuntimeFaulted());
    EXPECT_TRUE(disabled.IsSleeping()); EXPECT_VEC3_NEAR(disabled.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(ManagedPhysicsRuntimeTest, CachedFacadeCannotReviveAReAddedComponent)
{
    auto& target = m_scene.CreateGameObject("Target");
    Body(target);
    const std::string guid = target.instanceId;
    ASSERT_TRUE(Configure("{\"target\":\"" + guid + "\",\"useCached\":true}"));
    ASSERT_TRUE(Call(*m_adapter, scene::ScriptCallbackKind::AWAKE));
    ASSERT_TRUE(Fixed()); EXPECT_TRUE(Flag("bodyAlive"));
    target.RemoveComponent<scene::RigidBodyComponent>();
    ASSERT_TRUE(Fixed()); EXPECT_FALSE(Flag("bodyAlive")); EXPECT_FALSE(m_adapter->IsRuntimeFaulted());
    auto& replacementBody = Body(target);
    ASSERT_TRUE(Fixed()); EXPECT_FALSE(Flag("bodyAlive"));
    ASSERT_TRUE(Configure(R"({"operation":7,"value":[3,0,0]})")); EXPECT_FALSE(Fixed());
    EXPECT_TRUE(m_adapter->IsRuntimeFaulted());
    EXPECT_NE(m_adapter->FaultDiagnostic().find("expired"), std::string::npos);
    EXPECT_VEC3_NEAR(replacementBody.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(ManagedPhysicsRuntimeTest, ExplicitLookupReconnectsReAddedBodyButCannotReviveDestroyedGameObject)
{
    auto& target = m_scene.CreateGameObject("Target");
    Body(target);
    const auto id = target.GetID(); const std::string guid = target.instanceId;
    ASSERT_TRUE(Configure("{\"target\":\"" + guid + "\",\"useCached\":true}"));
    ASSERT_TRUE(Call(*m_adapter, scene::ScriptCallbackKind::AWAKE));
    target.RemoveComponent<scene::RigidBodyComponent>();
    auto& replacementBody = Body(target);
    ASSERT_TRUE(Configure(R"({"operation":7,"value":[3,0,0],"useCached":false})")); ASSERT_TRUE(Fixed());
    EXPECT_TRUE(Flag("bodyAlive")); EXPECT_VEC3_NEAR(replacementBody.GetVelocity(), (math::Vector3{3, 0, 0}), testkit::kTolerance);
    ASSERT_TRUE(Call(*m_adapter, scene::ScriptCallbackKind::AWAKE));
    ASSERT_TRUE(m_scene.DestroyGameObject(id)); m_scene.FlushDestroyQueue(0);
    auto& replacement = m_scene.CreateGameObject("SameGuid"); replacement.instanceId = guid;
    auto& newBody = Body(replacement);
    ASSERT_TRUE(Configure(R"({"operation":0})")); ASSERT_TRUE(Fixed()); EXPECT_FALSE(Flag("bodyAlive"));
    ASSERT_TRUE(Configure(R"({"operation":7,"useCached":true})")); EXPECT_FALSE(Fixed()); EXPECT_TRUE(m_adapter->IsRuntimeFaulted());
    EXPECT_VEC3_NEAR(newBody.GetVelocity(), math::Vector3::ZERO, testkit::kTolerance);
}

TEST_F(ManagedPhysicsRuntimeTest, RealWorldQueriesKeepAllRegisteredHitsBeyondTheFormerManagedCap)
{
    auto& target = m_scene.CreateGameObject("RepeatedWorldCollider"); auto& sphere = target.AddComponent<scene::SphereColliderComponent>();
    sphere.collider->Update({0, 0, 5}, math::Quaternion::Identity());
    physics::ColliderInstance instance; instance.collider = sphere.collider.get(); instance.material = &sphere.material;
    m_world.BeginSceneSync();
    for (unsigned index = 0; index < 32769; ++index) (void)m_world.SyncCollider({}, instance);
    m_world.EndSceneSync();
    ASSERT_TRUE(Call(*m_adapter, scene::ScriptCallbackKind::AWAKE));
    ASSERT_TRUE(Configure(R"({"operation":17,"runInUpdate":true,"maxDistance":100})"));
    ASSERT_TRUE(Call(*m_adapter, scene::ScriptCallbackKind::UPDATE));
    const auto results = ReferenceArray("hits"); ASSERT_EQ(results.size(), 32769u);
    EXPECT_EQ(results.front(), target.instanceId); EXPECT_EQ(results.back(), target.instanceId); EXPECT_FALSE(m_adapter->IsRuntimeFaulted());
}
} /// @note namespace fbzz::tests
