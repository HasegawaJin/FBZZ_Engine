/// @file    ManagedEngineNotificationTests.cpp
/// @brief   実 CoreCLR へ Engine 通知の値・権限・所有購読・世代と障害を転送する回帰。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include <Engine/Scene/ScriptComponent.hpp>
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Scheduler/SystemScheduler.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptEvent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Engine/Scene/Systems/NavigationSystem.hpp>
#include <Physics/World.hpp>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace fbzz::tests {

class ManagedEngineNotificationTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    physics::World m_world;
    SystemScheduler m_scheduler;
    scene::GameObject* m_owner = nullptr;
    scene::ManagedScriptAdapter* m_adapter = nullptr;
    bool m_previousPlay = false;
    size_t m_previousSubscriptions = 0;

    void SetUp() override
    {
        EngineFixture::SetUp();
        m_previousPlay = scene::Script::IsInPlayMode();
        scene::Script::SetInPlayMode(true);
        m_previousSubscriptions = scene::ScriptEventBus::SubscriptionCount();
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
        m_owner = &m_scene.CreateGameObject("EngineNotificationOwner");
        m_owner->AddComponent<scene::RigidBodyComponent>();
        m_adapter = Attach(*m_owner);
        ASSERT_NE(m_adapter, nullptr);
        m_scheduler.AddSystem<scene::ScriptSystem>();
        ASSERT_TRUE(m_scheduler.Build());
        Tick();
        ASSERT_FALSE(m_adapter->IsRuntimeFaulted()) << m_adapter->FaultDiagnostic();
    }
    void TearDown() override
    {
        m_scheduler.Shutdown();
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        runtime.Unload(&m_scene);
        m_scene.Clear();
        EXPECT_EQ(scene::ScriptEventBus::SubscriptionCount(), m_previousSubscriptions);
        EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic();
        scene::Script::SetInPlayMode(m_previousPlay);
        scene::Script::SetPhysicsWorld(nullptr);
        EngineFixture::TearDown();
    }
    void Tick()
    { m_scheduler.Update(SystemContext{m_scene, m_world, nullptr, nullptr, 0.02f, 0.02f, true, true}); }
    scene::ManagedScriptAdapter* Attach(scene::GameObject& object)
    {
        auto* scripts = object.GetComponent<scene::ScriptComponent>();
        if (!scripts) scripts = &object.AddComponent<scene::ScriptComponent>();
        auto& entry = scripts->scripts.emplace_back();
        entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.EngineNotificationComponent", entry.savedScriptId);
        if (!entry.script) return nullptr;
        entry.script->SetContext(&m_scene, &object);
        auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
        return adapter && adapter->RefreshOwnerBinding() ? adapter : nullptr;
    }
    scene::ManagedBridgeReply Read(scene::ManagedScriptAdapter& adapter, std::string_view field, std::string* text = nullptr)
    {
        scene::ManagedBridgeRequest request;
        request.op = scene::ManagedBridgeOp::READ_FIELD;
        request.generation = adapter.Generation(); request.instanceId = adapter.InstanceId();
        request.input = field.data(); request.inputBytes = static_cast<uint32_t>(field.size());
        scene::ManagedBridgeReply reply;
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        EXPECT_TRUE(runtime.Dispatch(request, reply, text)) << field << ": " << runtime.LastDiagnostic();
        return reply;
    }
    int Count(std::string_view field) { return static_cast<int>(Read(*m_adapter, field).integer); }
    std::string Text(std::string_view field)
    { std::string text; Read(*m_adapter, field, &text); return text; }
    math::Vector3 Vector(std::string_view field)
    { const auto value = Read(*m_adapter, field); return {value.x, value.y, value.z}; }
    bool Animation(scene::ManagedScriptAdapter& adapter, const scene::AnimationEventInfo& info)
    { return adapter.ExecuteProfiledCallback(&scene::Script::OnAnimationEvent, info, scene::ScriptCallbackKind::ANIMATION_EVENT, "managed animation"); }
    void PublishGlobal(const char* name = "Global Cue")
    {
        const scene::SequenceEvent event{name, -1073741823, 0.25f, 3.5f, "Global Sequence"};
        scene::ScriptEventBus::PublishRaw(scene::SequenceEvent::EVENT_NAME, &event);
    }
};

TEST_F(ManagedEngineNotificationTest, TransfersTypedValuesAndLiveScopeWithoutPhysicsOrFrameTimeAuthority)
{
    ASSERT_TRUE(m_adapter->ApplySettingsJson(R"({"verifyScope":true})"));
    const scene::AnimationEventInfo animation{"足音\"\\合図", std::numeric_limits<int32_t>::min(), 0.25f, 2.5f};
    ASSERT_TRUE(Animation(*m_adapter, animation));
    EXPECT_EQ(Text("eventName"), animation.name);
    EXPECT_EQ(Read(*m_adapter, "eventInteger").integer, animation.intParam);
    EXPECT_FLOAT_EQ(Read(*m_adapter, "eventScalar").x, animation.floatParam);
    EXPECT_FLOAT_EQ(Read(*m_adapter, "eventTime").x, animation.clipTime);
    scene::RootMotionInfo motion;
    motion.deltaPosition = {1, 2, 3}; motion.deltaRotation = {0, 0.6f, 0, 0.8f};
    motion.worldDeltaPosition = {4, 5, 6}; motion.worldVelocity = {7, 8, 9};
    motion.deltaTime = 0.125f; motion.appliedByEngine = false;
    ASSERT_TRUE(m_adapter->ExecuteProfiledCallback(&scene::Script::OnAnimatorMove, motion, scene::ScriptCallbackKind::ANIMATOR_MOVE, "managed root motion"));
    EXPECT_VEC3_NEAR(Vector("rootDelta"), motion.deltaPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(Vector("worldDelta"), motion.worldDeltaPosition, testkit::kTolerance);
    EXPECT_VEC3_NEAR(Vector("worldVelocity"), motion.worldVelocity, testkit::kTolerance);
    const auto rotation = Read(*m_adapter, "rootRotation");
    EXPECT_FLOAT_EQ(rotation.y, motion.deltaRotation.y); EXPECT_FLOAT_EQ(rotation.w, motion.deltaRotation.w);
    EXPECT_FLOAT_EQ(Read(*m_adapter, "rootSeconds").x, motion.deltaTime);
    EXPECT_FALSE(Read(*m_adapter, "rootApplied").integer);
    const scene::SequenceEventInfo sequence{"演出\"\\合図", std::numeric_limits<int32_t>::max(), -0.5f, 4.25f, "Sequence 名"};
    ASSERT_TRUE(m_adapter->ExecuteProfiledCallback(&scene::Script::OnSequenceEvent, sequence, scene::ScriptCallbackKind::SEQUENCE_EVENT, "managed sequence"));
    EXPECT_EQ(Text("eventName"), sequence.name); EXPECT_EQ(Text("sequenceName"), sequence.sequenceName);
    EXPECT_EQ(Read(*m_adapter, "eventInteger").integer, sequence.intParam);
    EXPECT_FLOAT_EQ(Read(*m_adapter, "eventScalar").x, sequence.floatParam);
    EXPECT_FLOAT_EQ(Read(*m_adapter, "eventTime").x, sequence.sequenceTime);
    ASSERT_TRUE(m_adapter->ExecuteProfiledCallback(&scene::Script::OnSequenceFinished, "Finished 名", scene::ScriptCallbackKind::SEQUENCE_FINISHED, "managed completion"));
    EXPECT_EQ(Text("sequenceName"), "Finished 名");
    ASSERT_TRUE(m_adapter->ExecuteProfiledCallback(&scene::Script::OnNavMeshDestinationReached, scene::ScriptCallbackKind::NAV_DESTINATION_REACHED, "managed arrival"));
    ASSERT_TRUE(m_adapter->ExecuteProfiledCallback(&scene::Script::OnNavMeshPathFailed, scene::ScriptCallbackKind::NAV_PATH_FAILED, "managed path failure"));
    ASSERT_TRUE(m_adapter->ExecuteProfiledCallback(&scene::Script::OnNavMeshTargetSpotted, scene::ScriptCallbackKind::NAV_TARGET_SPOTTED, "managed sight"));
    ASSERT_TRUE(m_adapter->ExecuteProfiledCallback(&scene::Script::OnNavMeshTargetLost, scene::ScriptCallbackKind::NAV_TARGET_LOST, "managed loss"));
    for (const char* field : {"animationCount", "motionCount", "sequenceCount", "finishedCount", "arrivedCount", "failedCount", "spottedCount", "lostCount"})
        EXPECT_EQ(Count(field), 1) << field;
    EXPECT_EQ(Count("timeRejectedCount"), 8); EXPECT_EQ(Count("physicsRejectedCount"), 8);
    EXPECT_VEC3_NEAR(m_owner->transform.position, (math::Vector3{8, 0, 0}), testkit::kTolerance);
    EXPECT_TRUE(scene::ManagedScriptRuntime::Instance().IsDispatchIdle());
}

TEST_F(ManagedEngineNotificationTest, NavigationCallbacksCanGrowAgentStorageWithoutLosingOriginalNotifications)
{
    auto& surfaceObject = m_scene.CreateGameObject("DisconnectedSurface");
    auto& surface = surfaceObject.AddComponent<scene::NavMeshSurfaceComponent>();
    scene::NavMeshPolygon start;
    start.vertices = {{-2, 0, -2}, {2, 0, -2}, {2, 0, 2}, {-2, 0, 2}};
    scene::NavMeshPolygon goal;
    goal.vertices = {{10, 0, -2}, {14, 0, -2}, {14, 0, 2}, {10, 0, 2}};
    surface.navMesh.SetPolygons({start, goal});
    m_owner->AddComponent<scene::NavMeshAgentComponent>().SetDestination({12, 0, 0});
    auto& second = m_scene.CreateGameObject("SecondNavigationOwner");
    second.AddComponent<scene::NavMeshAgentComponent>().SetDestination({12, 0, 0});
    auto* secondAdapter = Attach(second);
    ASSERT_NE(secondAdapter, nullptr);
    auto& third = m_scene.CreateGameObject("ThirdNavigationOwner");
    third.AddComponent<scene::NavMeshAgentComponent>().SetDestination({12, 0, 0});
    auto* thirdAdapter = Attach(third);
    ASSERT_NE(thirdAdapter, nullptr);
    Tick();
    ASSERT_TRUE(m_adapter->ApplySettingsJson(R"({"growAgentsOnPathFailure":true})"));
    SystemContext context{m_scene, m_world, nullptr, nullptr, 0.02f, 0.02f, true, true};
    scene::NavigationSystem navigation;
    navigation.Update(context);
    ASSERT_FALSE(m_adapter->IsRuntimeFaulted()) << m_adapter->FaultDiagnostic();
    EXPECT_EQ(Count("grownAgentCount"), 5001);
    EXPECT_EQ(Count("failedCount"), 1);
    EXPECT_EQ(Read(*secondAdapter, "failedCount").integer, 1);
    EXPECT_EQ(Read(*thirdAdapter, "failedCount").integer, 1);
    EXPECT_EQ(m_scene.GetEntities<scene::NavMeshAgentComponent>().size(), 5004u);
    navigation.Update(context);
    EXPECT_EQ(Count("failedCount"), 1);
    EXPECT_EQ(Read(*secondAdapter, "failedCount").integer, 1);
    EXPECT_EQ(Read(*thirdAdapter, "failedCount").integer, 1);
}

TEST_F(ManagedEngineNotificationTest, SkipsEditInactiveAndPendingRemovalNotifications)
{
    const scene::AnimationEventInfo info{"Cue", 1, 2, 3};
    scene::Script::SetInPlayMode(false);
    ASSERT_TRUE(Animation(*m_adapter, info));
    scene::Script::SetInPlayMode(true);
    m_owner->SetActive(false);
    ASSERT_TRUE(Animation(*m_adapter, info));
    m_owner->SetActive(true);
    auto& entry = m_owner->GetComponent<scene::ScriptComponent>()->scripts.front();
    entry.m_pendingRemoval = true;
    ASSERT_TRUE(Animation(*m_adapter, info));
    PublishGlobal();
    entry.m_pendingRemoval = false;
    EXPECT_EQ(Count("animationCount"), 0); EXPECT_EQ(Count("globalCount"), 0);
    ASSERT_TRUE(Animation(*m_adapter, info));
    EXPECT_EQ(Count("animationCount"), 1);
    EXPECT_FALSE(m_adapter->IsRuntimeFaulted());
}

TEST_F(ManagedEngineNotificationTest, OwnsBothSequenceChannelsThroughDisableEnableAndReload)
{
    ASSERT_EQ(scene::ScriptEventBus::SubscriptionCount(), m_previousSubscriptions + 2);
    ASSERT_TRUE(m_adapter->ApplySettingsJson(R"({"verifyScope":true})"));
    PublishGlobal("全体\"\\合図");
    EXPECT_EQ(Count("globalCount"), 1); EXPECT_EQ(Text("eventName"), "全体\"\\合図");
    EXPECT_EQ(Text("sequenceName"), "Global Sequence");
    EXPECT_EQ(Read(*m_adapter, "eventInteger").integer, -1073741823);
    EXPECT_EQ(Count("timeRejectedCount"), 1); EXPECT_EQ(Count("physicsRejectedCount"), 1);
    m_adapter->enabled = false; Tick();
    EXPECT_EQ(scene::ScriptEventBus::SubscriptionCount(), m_previousSubscriptions);
    PublishGlobal(); EXPECT_EQ(Count("globalCount"), 1);
    m_adapter->enabled = true; Tick();
    ASSERT_EQ(scene::ScriptEventBus::SubscriptionCount(), m_previousSubscriptions + 2);
    PublishGlobal(); EXPECT_EQ(Count("globalCount"), 2);
    const auto oldGeneration = m_adapter->Generation();
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Reload(m_scene, FBZZ_MANAGED_FIXTURE_V2_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    EXPECT_NE(m_adapter->Generation(), oldGeneration);
    EXPECT_EQ(scene::ScriptEventBus::SubscriptionCount(), m_previousSubscriptions + 2);
    PublishGlobal(); EXPECT_EQ(Count("globalCount"), 3);
    EXPECT_EQ(Count("connectCount"), 3);
    ASSERT_TRUE(m_adapter->ApplySettingsJson(R"({"publishManaged":true})"));
    Tick();
    EXPECT_EQ(Count("globalCount"), 4); EXPECT_EQ(Text("eventName"), "Managed Cue");
    EXPECT_EQ(Text("sequenceName"), "Managed Sequence");
    EXPECT_TRUE(runtime.IsDispatchIdle());
}

TEST_F(ManagedEngineNotificationTest, ManagedFailureReturnsFalsePreservesItsCauseAndCancelsBothSubscriptions)
{
    ASSERT_TRUE(m_adapter->ApplySettingsJson(R"({"throwOnAnimation":true})"));
    const scene::AnimationEventInfo info{"Cue", 1, 2, 3};
    EXPECT_FALSE(Animation(*m_adapter, info));
    EXPECT_TRUE(m_adapter->IsRuntimeFaulted()); EXPECT_FALSE(m_adapter->enabled);
    EXPECT_NE(m_adapter->FaultDiagnostic().find("Intentional Engine animation callback failure."), std::string::npos);
    EXPECT_EQ(scene::ScriptEventBus::SubscriptionCount(), m_previousSubscriptions);
    EXPECT_FALSE(Animation(*m_adapter, info));
    PublishGlobal(); EXPECT_EQ(Count("globalCount"), 0);
    EXPECT_EQ(Count("animationCount"), 1);
}

TEST_F(ManagedEngineNotificationTest, NativeGlobalFailureStopsOnlyItsReceiverAndReleasesItsChannels)
{
    auto& healthyObject = m_scene.CreateGameObject("HealthyReceiver");
    auto* healthy = Attach(healthyObject); ASSERT_NE(healthy, nullptr); Tick();
    ASSERT_EQ(scene::ScriptEventBus::SubscriptionCount(), m_previousSubscriptions + 4);
    ASSERT_TRUE(m_adapter->ApplySettingsJson(R"({"throwOnGlobal":true})"));
    PublishGlobal();
    EXPECT_TRUE(m_adapter->IsRuntimeFaulted()); EXPECT_FALSE(healthy->IsRuntimeFaulted());
    EXPECT_EQ(Read(*healthy, "globalCount").integer, 1);
    EXPECT_EQ(scene::ScriptEventBus::SubscriptionCount(), m_previousSubscriptions + 2);
    PublishGlobal();
    EXPECT_EQ(Count("globalCount"), 1); EXPECT_EQ(Read(*healthy, "globalCount").integer, 2);
    EXPECT_TRUE(scene::ManagedScriptRuntime::Instance().IsDispatchIdle());
}

TEST_F(ManagedEngineNotificationTest, RejectsInvalidNativePayloadWithoutExecutingManagedCode)
{
    const scene::AnimationEventInfo invalid{"Cue", 1, std::numeric_limits<float>::quiet_NaN(), 3};
    EXPECT_FALSE(Animation(*m_adapter, invalid));
    EXPECT_EQ(Count("animationCount"), 0);
    EXPECT_TRUE(m_adapter->IsRuntimeFaulted());
    EXPECT_NE(m_adapter->FaultDiagnostic().find("Animation notification contains invalid"), std::string::npos);
}

} /// @note namespace fbzz::tests
