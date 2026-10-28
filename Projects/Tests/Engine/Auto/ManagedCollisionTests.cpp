/// @file    ManagedCollisionTests.cpp
/// @brief   実 CoreCLR へ物理通知の値・向き・順序・寿命・障害を転送する回帰。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Core/Profiler/Profiler.hpp>
#include <Engine/Core/Scheduler/SystemScheduler.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Profiler/ScriptProfiler.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Physics/World.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace fbzz::tests {
namespace {

} /// @note namespace

class ManagedCollisionTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    physics::World m_world;
    SystemScheduler m_scheduler;
    scene::GameObject* m_owner = nullptr;
    scene::GameObject* m_other = nullptr;
    scene::ManagedScriptAdapter* m_adapter = nullptr;
    float m_previousDelta = 0;
    float m_previousFixed = 0;
    bool m_previousPlay = false;
    bool m_previousProfiler = false;
    bool m_previousRecording = false;

    void SetUp() override
    {
        EngineFixture::SetUp();
        m_previousDelta = Time::deltaTime;
        m_previousFixed = Time::fixedDeltaTime;
        m_previousPlay = scene::Script::IsInPlayMode();
        m_previousProfiler = profiler::Profiler::IsEnabled();
        m_previousRecording = scene::ScriptProfiler::IsRecording();
        Time::deltaTime = 0.02f;
        Time::fixedDeltaTime = 0.02f;
        scene::Script::SetInPlayMode(true);
        m_world.SetGravity(math::Vector3::ZERO);
        m_world.SetSubsteps(1);
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
        m_owner = &m_scene.CreateGameObject("CollisionOwner");
        m_other = &m_scene.CreateGameObject("CollisionOther");
        m_adapter = Attach(*m_owner);
        ASSERT_NE(m_adapter, nullptr);
        m_scheduler.AddSystem<scene::ScriptSystem>();
        m_scheduler.AddSystem<scene::PhysicsSystem>();
        m_scheduler.Build();
    }

    void TearDown() override
    {
        m_scheduler.Shutdown();
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        runtime.Unload(&m_scene);
        m_scene.Clear();
        m_adapter = nullptr;
        m_owner = nullptr;
        m_other = nullptr;
        EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic();
        scene::Script::SetPhysicsWorld(nullptr);
        scene::Script::SetInPlayMode(m_previousPlay);
        Time::deltaTime = m_previousDelta;
        Time::fixedDeltaTime = m_previousFixed;
        scene::ScriptProfiler::RequestClear();
        scene::ScriptProfiler::RequestRecording(m_previousRecording);
        scene::ScriptProfiler::BeginFrame(2, 0.0);
        scene::ScriptProfiler::EndFrame();
        profiler::Profiler::SetEnabled(m_previousProfiler);
        EngineFixture::TearDown();
    }

    scene::ManagedScriptAdapter* Attach(scene::GameObject& object)
    {
        auto* component = object.GetComponent<scene::ScriptComponent>();
        if (!component) component = &object.AddComponent<scene::ScriptComponent>();
        auto& entry = component->scripts.emplace_back();
    entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.CollisionComponent", entry.savedScriptId);
        if (!entry.script) return nullptr;
        entry.script->SetContext(&m_scene, &object);
        auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
        return adapter && adapter->RefreshOwnerBinding() ? adapter : nullptr;
    }

    scene::ManagedBridgeReply Read(scene::ManagedScriptAdapter& adapter, std::string_view field, std::string* text = nullptr)
    {
        scene::ManagedBridgeRequest request;
        request.op = scene::ManagedBridgeOp::READ_FIELD;
        request.generation = adapter.Generation();
        request.instanceId = adapter.InstanceId();
        request.input = field.data();
        request.inputBytes = static_cast<uint32_t>(field.size());
        scene::ManagedBridgeReply reply;
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        EXPECT_TRUE(runtime.Dispatch(request, reply, text)) << field << ": " << runtime.LastDiagnostic();
        return reply;
    }
    int Count(scene::ManagedScriptAdapter& adapter, std::string_view field)
    { return static_cast<int>(Read(adapter, field).integer); }
    math::Vector3 Vector(scene::ManagedScriptAdapter& adapter, std::string_view field)
    { const auto value = Read(adapter, field); return {value.x, value.y, value.z}; }
    void Tick(bool simulating)
    { m_scheduler.Update(SystemContext{m_scene, m_world, nullptr, nullptr, 0.02f, 0.02f, simulating, simulating}); }
    bool Call(void (scene::Script::*callback)(const scene::CollisionInfo&), const scene::CollisionInfo& collision,
              scene::ScriptCallbackKind kind)
    { return m_adapter->ExecuteProfiledCallback(callback, collision, kind, "managed collision"); }
    scene::CollisionInfo Snapshot()
    {
        scene::CollisionInfo collision;
        collision.self = m_owner;
        collision.other = m_other;
        collision.contactPoint = {1, 2, 3};
        collision.contactNormal = {-1, 0, 0};
        collision.contactDepth = 0.5f;
        collision.relativeVelocity = {4, 5, 6};
        collision.approachSpeed = 3;
        collision.impactImpulse = 2;
        return collision;
    }
};

TEST_F(ManagedCollisionTest, TransfersSixTypedCallbacksAndNullTargetWithCurrentScope)
{
    ASSERT_TRUE(m_adapter->ApplySettingsJson(R"({"readTime":true,"touchTransform":true})"));
    const auto collision = Snapshot();
    ASSERT_TRUE(Call(&scene::Script::OnCollisionEnter, collision, scene::ScriptCallbackKind::COLLISION_ENTER));
    EXPECT_VEC3_NEAR(Vector(*m_adapter, "point"), collision.contactPoint, testkit::kTolerance);
    EXPECT_VEC3_NEAR(Vector(*m_adapter, "normal"), collision.contactNormal, testkit::kTolerance);
    EXPECT_VEC3_NEAR(Vector(*m_adapter, "relativeVelocity"), collision.relativeVelocity, testkit::kTolerance);
    EXPECT_FLOAT_EQ(Read(*m_adapter, "depth").x, collision.contactDepth);
    EXPECT_FLOAT_EQ(Read(*m_adapter, "approachSpeed").x, collision.approachSpeed);
    EXPECT_FLOAT_EQ(Read(*m_adapter, "impulse").x, collision.impactImpulse);
    EXPECT_FLOAT_EQ(Read(*m_adapter, "deltaSeconds").x, Time::fixedDeltaTime);
    std::string otherGuid;
    Read(*m_adapter, "other", &otherGuid);
    EXPECT_EQ(otherGuid, m_other->instanceId);
    ASSERT_TRUE(Call(&scene::Script::OnCollisionStay, collision, scene::ScriptCallbackKind::COLLISION_STAY));
    ASSERT_TRUE(Call(&scene::Script::OnCollisionExit, collision, scene::ScriptCallbackKind::COLLISION_EXIT));
    ASSERT_TRUE(Call(&scene::Script::OnTriggerEnter, collision, scene::ScriptCallbackKind::TRIGGER_ENTER));
    ASSERT_TRUE(Call(&scene::Script::OnTriggerStay, collision, scene::ScriptCallbackKind::TRIGGER_STAY));
    ASSERT_TRUE(Call(&scene::Script::OnTriggerExit, collision, scene::ScriptCallbackKind::TRIGGER_EXIT));
    for (const char* field : {"collisionEnterCount", "collisionStayCount", "collisionExitCount", "triggerEnterCount", "triggerStayCount", "triggerExitCount"})
        EXPECT_EQ(Count(*m_adapter, field), 1) << field;
    EXPECT_VEC3_NEAR(m_owner->transform.position, (math::Vector3{6, 0, 0}), testkit::kTolerance);
    scene::CollisionInfo absent = collision;
    absent.other = nullptr;
    ASSERT_TRUE(Call(&scene::Script::OnTriggerExit, absent, scene::ScriptCallbackKind::TRIGGER_EXIT));
    EXPECT_FALSE(Read(*m_adapter, "otherAlive").integer);
    const auto nullValue = Read(*m_adapter, "other", &otherGuid);
    EXPECT_TRUE(nullValue.flags & scene::kManagedNullValue);
    EXPECT_TRUE(otherGuid.empty());
}

TEST_F(ManagedCollisionTest, ActualPhysicsDeliversReciprocalManagedValuesAndSkipsEditNotifications)
{
    auto* otherAdapter = Attach(*m_other);
    ASSERT_NE(otherAdapter, nullptr);
    m_owner->AddComponent<scene::AabbColliderComponent>();
    m_other->AddComponent<scene::AabbColliderComponent>();
    auto& body = m_owner->AddComponent<scene::RigidBodyComponent>();
    body.rigidBody = std::make_unique<physics::RigidBody>();
    body.rigidBody->SetMass(1);
    body.rigidBody->SetFreezePosition({true, true, true});
    body.rigidBody->SetFreezeRotation({true, true, true});
    m_other->transform.position = {0.5f, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    Tick(false);
    EXPECT_EQ(Count(*m_adapter, "collisionEnterCount"), 0);
    Tick(true);
    ASSERT_EQ(Count(*m_adapter, "collisionEnterCount"), 1);
    EXPECT_VEC3_NEAR(Vector(*m_adapter, "normal"), -Vector(*otherAdapter, "normal"), testkit::kTolerance);
    EXPECT_VEC3_NEAR(Vector(*m_adapter, "relativeVelocity"), -Vector(*otherAdapter, "relativeVelocity"), testkit::kTolerance);
    Tick(true);
    EXPECT_EQ(Count(*m_adapter, "collisionStayCount"), 1);
    EXPECT_FLOAT_EQ(Read(*m_adapter, "approachSpeed").x, 0);
    EXPECT_FLOAT_EQ(Read(*m_adapter, "impulse").x, 0);
    m_other->transform.position = {3, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    Tick(true);
    EXPECT_EQ(Count(*m_adapter, "collisionExitCount"), 1);
}

TEST_F(ManagedCollisionTest, ActualTriggerDeliversEnterStayExitToBothObjects)
{
    auto* otherAdapter = Attach(*m_other);
    ASSERT_NE(otherAdapter, nullptr);
    m_owner->AddComponent<scene::AabbColliderComponent>().isTrigger = true;
    m_other->AddComponent<scene::AabbColliderComponent>();
    /// @note 静的な領域へ出入りする動的対象を固定 step で同期し、native と managed の同じイベント分類を検証する。
    auto& body = m_other->AddComponent<scene::RigidBodyComponent>();
    body.rigidBody = std::make_unique<physics::RigidBody>();
    body.rigidBody->SetMass(1);
    /// @note SetPosition と Teleport は固定軸も維持するため、出入りに使う X 軸だけは固定しない。
    body.rigidBody->SetFreezePosition({false, true, true});
    body.rigidBody->SetFreezeRotation({true, true, true});
    m_other->transform.position = {0.5f, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    Tick(false);
    EXPECT_EQ(Count(*m_adapter, "triggerEnterCount"), 0);
    Tick(true);
    EXPECT_VEC3_NEAR(body.rigidBody->GetPosition(), (math::Vector3{0.5f, 0, 0}), testkit::kTolerance);
    ASSERT_EQ(m_world.GetEnterEvents().size(), 1u);
    EXPECT_TRUE(m_world.GetEnterEvents().front().isTrigger);
    EXPECT_TRUE(m_world.GetStayEvents().empty());
    EXPECT_TRUE(m_world.GetExitEvents().empty());
    Tick(true);
    ASSERT_EQ(m_world.GetStayEvents().size(), 1u);
    EXPECT_TRUE(m_world.GetEnterEvents().empty());
    EXPECT_TRUE(m_world.GetExitEvents().empty());
    m_other->transform.position = {3, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    Tick(true);
    EXPECT_VEC3_NEAR(body.rigidBody->GetPosition(), (math::Vector3{3, 0, 0}), testkit::kTolerance);
    ASSERT_EQ(m_world.GetExitEvents().size(), 1u);
    EXPECT_TRUE(m_world.GetExitEvents().front().isTrigger);
    EXPECT_TRUE(m_world.GetEnterEvents().empty());
    EXPECT_TRUE(m_world.GetStayEvents().empty());
    for (auto* adapter : {m_adapter, otherAdapter}) {
        EXPECT_EQ(Count(*adapter, "triggerEnterCount"), 1);
        EXPECT_EQ(Count(*adapter, "triggerStayCount"), 1);
        EXPECT_EQ(Count(*adapter, "triggerExitCount"), 1);
        EXPECT_EQ(Count(*adapter, "collisionEnterCount"), 0);
    }
    m_adapter->enabled = false;
    m_other->transform.position = {0.5f, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    Tick(true);
    EXPECT_VEC3_NEAR(body.rigidBody->GetPosition(), (math::Vector3{0.5f, 0, 0}), testkit::kTolerance);
    ASSERT_EQ(m_world.GetEnterEvents().size(), 1u);
    EXPECT_TRUE(m_world.GetEnterEvents().front().isTrigger);
    EXPECT_EQ(Count(*m_adapter, "triggerEnterCount"), 1);
    EXPECT_EQ(Count(*otherAdapter, "triggerEnterCount"), 2);
}

TEST_F(ManagedCollisionTest, StaticOnlyTriggerMovementUpdatesNativeAndManagedContactPhases)
{
    auto* otherAdapter = Attach(*m_other);
    ASSERT_NE(otherAdapter, nullptr);
    m_owner->AddComponent<scene::AabbColliderComponent>().isTrigger = true;
    m_other->AddComponent<scene::AabbColliderComponent>();
    ASSERT_EQ(m_owner->GetComponent<scene::RigidBodyComponent>(), nullptr);
    ASSERT_EQ(m_other->GetComponent<scene::RigidBodyComponent>(), nullptr);
    m_other->transform.position = {0.5f, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    Tick(false);
    for (auto* adapter : {m_adapter, otherAdapter})
        EXPECT_EQ(Count(*adapter, "triggerEnterCount"), 0);

    Tick(true);
    ASSERT_EQ(m_world.GetEnterEvents().size(), 1u);
    EXPECT_TRUE(m_world.GetEnterEvents().front().isTrigger);
    EXPECT_TRUE(m_world.GetStayEvents().empty());
    EXPECT_TRUE(m_world.GetExitEvents().empty());
    for (auto* adapter : {m_adapter, otherAdapter}) {
        EXPECT_EQ(Count(*adapter, "triggerEnterCount"), 1);
        EXPECT_EQ(Count(*adapter, "triggerStayCount"), 0);
        EXPECT_EQ(Count(*adapter, "triggerExitCount"), 0);
    }

    Tick(true);
    ASSERT_EQ(m_world.GetStayEvents().size(), 1u);
    EXPECT_TRUE(m_world.GetStayEvents().front().isTrigger);
    EXPECT_TRUE(m_world.GetEnterEvents().empty());
    EXPECT_TRUE(m_world.GetExitEvents().empty());
    for (auto* adapter : {m_adapter, otherAdapter})
        EXPECT_EQ(Count(*adapter, "triggerStayCount"), 1);

    /// @note 既存 collider の姿勢変更でも接触を再検出し、静止シーンの早期 return が古い Stay を再送しないこと。
    m_other->transform.position = {3, 0, 0};
    scene::FlushWorldTransforms(m_scene);
    Tick(true);
    ASSERT_EQ(m_world.GetExitEvents().size(), 1u);
    EXPECT_TRUE(m_world.GetExitEvents().front().isTrigger);
    EXPECT_TRUE(m_world.GetEnterEvents().empty());
    EXPECT_TRUE(m_world.GetStayEvents().empty());
    for (auto* adapter : {m_adapter, otherAdapter}) {
        EXPECT_EQ(Count(*adapter, "triggerEnterCount"), 1);
        EXPECT_EQ(Count(*adapter, "triggerStayCount"), 1);
        EXPECT_EQ(Count(*adapter, "triggerExitCount"), 1);
        EXPECT_EQ(Count(*adapter, "collisionEnterCount"), 0);
        EXPECT_FALSE(adapter->IsRuntimeFaulted());
    }
}

TEST_F(ManagedCollisionTest, ManagedExceptionFaultsOnlyItsEntryAndProfilerReportsFailure)
{
    auto* healthy = Attach(*m_owner);
    ASSERT_NE(healthy, nullptr);
    ASSERT_TRUE(m_adapter->ApplySettingsJson(R"({"throwOnEnter":true})"));
    profiler::Profiler::SetEnabled(true);
    scene::ScriptProfiler::RequestClear();
    scene::ScriptProfiler::RequestRecording(true);
    profiler::Profiler::BeginFrame(1, 0.0);
    scene::ScriptProfiler::BeginFrame(1, 0.0);
    const auto collision = Snapshot();
    EXPECT_FALSE(Call(&scene::Script::OnCollisionEnter, collision, scene::ScriptCallbackKind::COLLISION_ENTER));
    EXPECT_TRUE(m_adapter->IsRuntimeFaulted());
    EXPECT_FALSE(m_adapter->enabled);
    EXPECT_FALSE(Call(&scene::Script::OnCollisionEnter, collision, scene::ScriptCallbackKind::COLLISION_ENTER));
    EXPECT_EQ(Count(*m_adapter, "collisionEnterCount"), 1);
    scene::ScriptProfiler::EndFrame();
    profiler::Profiler::EndFrame();
    const auto& samples = scene::ScriptProfiler::GetSnapshot().samples;
    ASSERT_EQ(samples.size(), 1u);
    EXPECT_EQ(samples.front().status, profiler::ProfileSampleStatus::FAULTED);
    const std::string failure = m_adapter->FaultDiagnostic();
    EXPECT_NE(failure.find("Intentional collision callback failure."), std::string::npos);
    ASSERT_TRUE(healthy->ExecuteProfiledCallback(&scene::Script::OnTriggerEnter, collision,
        scene::ScriptCallbackKind::TRIGGER_ENTER, "healthy trigger"));
    EXPECT_EQ(Count(*healthy, "triggerEnterCount"), 1);
    EXPECT_EQ(m_adapter->FaultDiagnostic(), failure);
}

TEST_F(ManagedCollisionTest, RetainedOtherDoesNotReviveWhenTheSameGuidIsRecreated)
{
    ASSERT_TRUE(Call(&scene::Script::OnCollisionEnter, Snapshot(), scene::ScriptCallbackKind::COLLISION_ENTER));
    const std::string guid = m_other->instanceId;
    const scene::EntityID oldEntity = m_other->GetID();
    ASSERT_TRUE(m_scene.DestroyGameObject(oldEntity));
    m_other = &m_scene.CreateGameObject("Replacement");
    m_other->instanceId = guid;
    ASSERT_TRUE(m_adapter->ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "cached target"));
    EXPECT_FALSE(Read(*m_adapter, "cachedOtherAlive").integer);
    ASSERT_TRUE(m_adapter->ApplySettingsJson(R"({"attemptCachedMutation":true})"));
    EXPECT_FALSE(m_adapter->ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "expired target"));
    EXPECT_TRUE(m_adapter->IsRuntimeFaulted());
    EXPECT_VEC3_NEAR(m_other->transform.position, math::Vector3::ZERO, testkit::kTolerance);
}

} /// @note namespace fbzz::tests
