/// @file    ManagedCompletionTests.cpp
/// @brief   実 CoreCLR に接続した Play reload・jobs・同期イベント・複合保存の回帰。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptEvent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/ScriptValidation.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/VFXComponent.hpp>
#include <Engine/Scene/Components/SequencePlayerComponent.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/TomlReflector.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Physics/World.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedFieldCodec.hpp"
#include <array>
#include <limits>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace fbzz::tests {
class ManagedCompletionTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_play = scene::Script::IsInPlayMode(); m_delta = Time::deltaTime; m_unscaled = Time::unscaledDeltaTime;
        scene::Script::SetInPlayMode(false);
        Time::deltaTime = Time::unscaledDeltaTime = 0.25f;
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
        ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    }
    void TearDown() override
    {
        auto& runtime = scene::ManagedScriptRuntime::Instance();
        runtime.Unload(&m_scene); m_scene.Clear();
        EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic();
        scene::Script::SetInPlayMode(m_play); Time::deltaTime = m_delta; Time::unscaledDeltaTime = m_unscaled;
        EngineFixture::TearDown();
    }
    scene::ManagedScriptAdapter* Add(scene::GameObject& object, const char* type)
    {
        auto* component = object.GetComponent<scene::ScriptComponent>();
        if (!component) component = &object.AddComponent<scene::ScriptComponent>();
        auto& entry = component->scripts.emplace_back();
    entry.script = scene::ScriptFactory::Create(type, entry.savedScriptId);
        if (!entry.script) return nullptr;
        entry.script->SetContext(&m_scene, &object);
        auto* adapter = scene::ManagedScriptRuntime::Instance().FindAdapter(entry.script.get());
        return adapter && adapter->RefreshOwnerBinding() ? adapter : nullptr;
    }
    void Tick()
    { SystemContext context{m_scene, m_world, nullptr, nullptr, 0.25f, 0.02f, true, true}; scene::ScriptSystem{}.Update(context); }
    scene::ManagedBridgeReply Read(scene::ManagedScriptAdapter& adapter, std::string_view field)
    {
        scene::ManagedBridgeRequest request;
        request.op = scene::ManagedBridgeOp::READ_FIELD; request.generation = adapter.Generation(); request.instanceId = adapter.InstanceId();
        request.ownerId = adapter.OwnerId(); request.input = field.data(); request.inputBytes = static_cast<uint32_t>(field.size());
        scene::ManagedBridgeReply reply;
        EXPECT_TRUE(scene::ManagedScriptRuntime::Instance().Dispatch(request, reply)) << field;
        return reply;
    }
    std::string ReadText(scene::ManagedScriptAdapter& adapter, std::string_view field)
    {
        scene::ManagedBridgeRequest request;
        request.op = scene::ManagedBridgeOp::READ_FIELD; request.generation = adapter.Generation(); request.instanceId = adapter.InstanceId();
        request.ownerId = adapter.OwnerId(); request.input = field.data(); request.inputBytes = static_cast<uint32_t>(field.size());
        scene::ManagedBridgeReply reply;
        std::string text;
        EXPECT_TRUE(scene::ManagedScriptRuntime::Instance().Dispatch(request, reply, &text)) << field;
        return text;
    }
    scene::Scene m_scene;
    physics::World m_world;
    testkit::TempDir m_temp{"managed-completion"};
    bool m_play = false;
    float m_delta = 0;
    float m_unscaled = 0;
};

TEST_F(ManagedCompletionTest, OwnedJobsAndSynchronousEventsUseTheRealHostAndStopOnDisable)
{
    auto& caller = m_scene.CreateGameObject("Caller");
    auto& target = m_scene.CreateGameObject("Target");
    auto* first = Add(caller, "cs-internal:ManagedFixtures.CompletionComponent");
    auto* second = Add(target, "cs-internal:ManagedFixtures.CompletionComponent");
    ASSERT_NE(first, nullptr); ASSERT_NE(second, nullptr);
    Tick();
    EXPECT_EQ(Read(*first, "connectCount").integer, 1);
    EXPECT_EQ(Read(*second, "connectCount").integer, 1);
    ASSERT_TRUE(first->ApplySettingsJson(R"({"command":1})"));
    Tick();
    EXPECT_EQ(Read(*first, "jobCount").integer, 0);
    Tick();
    EXPECT_NEAR(Read(*first, "tweenValue").x, 0.5f, testkit::kTolerance);
    EXPECT_EQ(Read(*first, "frameCount").integer, 0);
    Tick();
    EXPECT_EQ(Read(*first, "jobCount").integer, 1);
    EXPECT_EQ(Read(*first, "frameCount").integer, 1);
    EXPECT_EQ(Read(*first, "tweenCompleted").integer, 1);
    ASSERT_TRUE(first->ApplySettingsJson(R"({"command":3})"));
    Tick();
    EXPECT_EQ(Read(*first, "eventSum").integer, 3);
    EXPECT_EQ(Read(*second, "eventSum").integer, 3);
    EXPECT_NEAR(caller.transform.position.x, 3.0f, testkit::kTolerance);
    EXPECT_NEAR(target.transform.position.x, 3.0f, testkit::kTolerance);
    second->enabled = false;
    ASSERT_TRUE(first->ApplySettingsJson(R"({"command":3})"));
    Tick();
    EXPECT_EQ(Read(*first, "eventSum").integer, 6);
    EXPECT_EQ(Read(*second, "eventSum").integer, 3);
    second->enabled = true;
    Tick();
    EXPECT_EQ(Read(*second, "connectCount").integer, 2);
    ASSERT_TRUE(first->ApplySettingsJson(R"({"command":3})"));
    Tick(); Tick();
    EXPECT_EQ(Read(*first, "eventSum").integer, 9);
    EXPECT_EQ(Read(*second, "eventSum").integer, 6);
    EXPECT_EQ(Read(*second, "connectCount").integer, 2);
    ASSERT_TRUE(first->ApplySettingsJson(R"({"command":2})"));
    Tick();
    for (int index = 0; index < 6; ++index) Tick();
    EXPECT_EQ(Read(*first, "timelineCount").integer, 2);
}

TEST_F(ManagedCompletionTest, PlayReloadKeepsStateAndEntryFlagsWithoutRepeatingAwakeStart)
{
    auto& object = m_scene.CreateGameObject("Owner");
    auto& target = m_scene.CreateGameObject("Target");
    auto& disabledObject = m_scene.CreateGameObject("Disabled");
    auto* health = Add(target, "cs-internal:ManagedFixtures.HealthComponent");
    auto* disabled = Add(disabledObject, "cs-internal:ManagedFixtures.CompletionComponent");
    ASSERT_NE(health, nullptr); ASSERT_NE(disabled, nullptr);
    auto* adapter = Add(object, "cs-internal:ManagedFixtures.CompletionComponent");
    ASSERT_NE(adapter, nullptr);
    Tick();
    auto& entry = object.GetComponent<scene::ScriptComponent>()->scripts.front();
    const auto oldGeneration = adapter->Generation();
    const auto oldInstance = adapter->InstanceId();
    ASSERT_TRUE(entry.m_awoken && entry.m_started);
    disabled->enabled = false;
    ASSERT_TRUE(adapter->ApplySettingsJson(R"({"command":1})"));
    Tick();
    const float elapsed = Read(*adapter, "m_elapsed").x;
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_FALSE(runtime.Reload(m_scene, FBZZ_MANAGED_INVALID_TEST_ASSEMBLY, true));
    EXPECT_FALSE(runtime.LastDiagnostic().empty());
    EXPECT_EQ(adapter->Generation(), oldGeneration); EXPECT_EQ(adapter->InstanceId(), oldInstance);
    EXPECT_TRUE(adapter->enabled); EXPECT_FALSE(disabled->enabled);
    EXPECT_TRUE(entry.m_awoken && entry.m_started);
    ASSERT_TRUE(runtime.Reload(m_scene, FBZZ_MANAGED_FIXTURE_V2_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    EXPECT_EQ(entry.script.get(), adapter);
    EXPECT_NE(adapter->Generation(), oldGeneration); EXPECT_NE(adapter->InstanceId(), oldInstance);
    EXPECT_TRUE(entry.m_awoken && entry.m_started);
    EXPECT_EQ(Read(*adapter, "awakeCount").integer, 1); EXPECT_EQ(Read(*adapter, "startCount").integer, 1);
    EXPECT_NEAR(Read(*adapter, "m_elapsed").x, elapsed, testkit::kTolerance);
    EXPECT_NEAR(Read(*adapter, "ManagedFixtures.CompletionComponent.m_transient").x, 0, testkit::kTolerance);
    EXPECT_EQ(Read(*adapter, "afterReloadCount").integer, 1);
    EXPECT_EQ(Read(*adapter, "connectCount").integer, 2);
    EXPECT_FALSE(disabled->enabled);
    EXPECT_EQ(Read(*disabled, "awakeCount").integer, 1); EXPECT_EQ(Read(*disabled, "startCount").integer, 1);
    EXPECT_EQ(Read(*disabled, "connectCount").integer, 1);
    ASSERT_TRUE(adapter->ApplySettingsJson(R"({"command":4})"));
    Tick();
    EXPECT_EQ(Read(*adapter, "cacheChecks").integer, 1); EXPECT_EQ(Read(*adapter, "cacheFailures").integer, 0);
    EXPECT_FALSE(Read(*adapter, "cacheScriptExpired").integer);
    EXPECT_NEAR(target.transform.position.x, 2, testkit::kTolerance);
    EXPECT_EQ(Read(*health, "health").integer, 9);
    ASSERT_TRUE(adapter->ApplySettingsJson(R"({"command":3})"));
    Tick();
    EXPECT_EQ(Read(*adapter, "eventSum").integer, 3); EXPECT_EQ(Read(*disabled, "eventSum").integer, 0);
    EXPECT_EQ(Read(*adapter, "connectCount").integer, 2);
    disabled->enabled = true;
    Tick();
    EXPECT_EQ(Read(*disabled, "connectCount").integer, 2);
    ASSERT_TRUE(adapter->ApplySettingsJson(R"({"command":3})"));
    Tick();
    EXPECT_EQ(Read(*adapter, "eventSum").integer, 6); EXPECT_EQ(Read(*disabled, "eventSum").integer, 3);
    EXPECT_EQ(Read(*adapter, "connectCount").integer, 2); EXPECT_EQ(Read(*disabled, "connectCount").integer, 2);
    const auto currentGeneration = adapter->Generation();
    ASSERT_FALSE(runtime.Reload(m_scene, FBZZ_MANAGED_INVALID_TEST_ASSEMBLY, true));
    EXPECT_FALSE(runtime.LastDiagnostic().empty());
    EXPECT_EQ(adapter->Generation(), currentGeneration);
    Tick(); Tick(); Tick();
    EXPECT_EQ(Read(*adapter, "awakeCount").integer, 1); EXPECT_EQ(Read(*adapter, "startCount").integer, 1);
    EXPECT_EQ(Read(*adapter, "jobCount").integer, 0);
}

TEST_F(ManagedCompletionTest, PlayReloadResolvesEntriesAfterAddingScriptsToTheSameAndLaterObjects)
{
    auto& sameObject = m_scene.CreateGameObject("Same");
    auto& callerObject = m_scene.CreateGameObject("Caller");
    auto& laterObject = m_scene.CreateGameObject("Later");
    auto* sameCaller = Add(sameObject, "cs-internal:ManagedFixtures.CompletionComponent");
    auto* sameObserver = Add(sameObject, "cs-internal:ManagedFixtures.CompletionComponent");
    auto* caller = Add(callerObject, "cs-internal:ManagedFixtures.CompletionComponent");
    auto* laterObserver = Add(laterObject, "cs-internal:ManagedFixtures.CompletionComponent");
    ASSERT_NE(sameCaller, nullptr); ASSERT_NE(sameObserver, nullptr);
    ASSERT_NE(caller, nullptr); ASSERT_NE(laterObserver, nullptr);
    Tick();
    const std::array originals{sameCaller, sameObserver, caller, laterObserver};
    auto& sameEntries = sameObject.GetComponent<scene::ScriptComponent>()->scripts;
    auto& laterEntries = laterObject.GetComponent<scene::ScriptComponent>()->scripts;
    sameEntries.resize(sameEntries.capacity());
    laterEntries.resize(laterEntries.capacity());
    const auto sameCapacity = sameEntries.capacity();
    const auto laterCapacity = laterEntries.capacity();
    const auto sameIdentity = sameEntries.front().inspectionId;
    const auto laterIdentity = laterEntries.front().inspectionId;
    const auto entities = m_scene.GetEntities<scene::ScriptComponent>();
    ASSERT_EQ(entities.size(), 3);
    ASSERT_EQ(entities[1], callerObject.GetID()); ASSERT_EQ(entities[2], laterObject.GetID());
    ASSERT_TRUE(sameCaller->ApplySettingsJson(R"({"reloadAddCount":1})"));
    ASSERT_TRUE(caller->ApplySettingsJson("{\"reloadAddCount\":1,\"reloadAddTarget\":\"" + laterObject.instanceId + "\"}"));

    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Reload(m_scene, FBZZ_MANAGED_FIXTURE_V2_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();

    EXPECT_GT(sameEntries.capacity(), sameCapacity); EXPECT_GT(laterEntries.capacity(), laterCapacity);
    ASSERT_EQ(sameEntries.size(), sameCapacity + 1); ASSERT_EQ(laterEntries.size(), laterCapacity + 1);
    EXPECT_EQ(sameEntries.front().inspectionId, sameIdentity); EXPECT_EQ(laterEntries.front().inspectionId, laterIdentity);
    EXPECT_EQ(Read(*sameCaller, "reloadAddedCount").integer, 1); EXPECT_EQ(Read(*caller, "reloadAddedCount").integer, 1);
    for (auto* adapter : originals) {
        EXPECT_FALSE(adapter->IsRuntimeFaulted()) << adapter->FaultDiagnostic();
        EXPECT_EQ(Read(*adapter, "afterReloadCount").integer, 1);
        EXPECT_EQ(Read(*adapter, "connectCount").integer, 2);
        EXPECT_EQ(Read(*adapter, "awakeCount").integer, 1); EXPECT_EQ(Read(*adapter, "startCount").integer, 1);
    }
    EXPECT_TRUE(sameEntries.front().m_awoken && sameEntries.front().m_started);
    EXPECT_TRUE(sameEntries[1].m_awoken && sameEntries[1].m_started);
    EXPECT_TRUE(laterEntries.front().m_awoken && laterEntries.front().m_started);
    auto* sameAdded = runtime.FindAdapter(sameEntries.back().script.get());
    auto* laterAdded = runtime.FindAdapter(laterEntries.back().script.get());
    ASSERT_NE(sameAdded, nullptr); ASSERT_NE(laterAdded, nullptr);
    const std::array added{sameAdded, laterAdded};
    for (auto* adapter : added) {
        EXPECT_EQ(Read(*adapter, "afterReloadCount").integer, 0);
        EXPECT_EQ(Read(*adapter, "awakeCount").integer, 0); EXPECT_EQ(Read(*adapter, "startCount").integer, 0);
        EXPECT_EQ(Read(*adapter, "connectCount").integer, 0);
    }
    EXPECT_FALSE(sameEntries.back().m_awoken || sameEntries.back().m_started);
    EXPECT_FALSE(laterEntries.back().m_awoken || laterEntries.back().m_started);
    Tick();
    for (auto* adapter : added) {
        EXPECT_FALSE(adapter->IsRuntimeFaulted()) << adapter->FaultDiagnostic();
        EXPECT_EQ(Read(*adapter, "awakeCount").integer, 1); EXPECT_EQ(Read(*adapter, "startCount").integer, 1);
        EXPECT_EQ(Read(*adapter, "connectCount").integer, 1);
    }
    for (auto* adapter : originals) {
        EXPECT_EQ(Read(*adapter, "afterReloadCount").integer, 1);
        EXPECT_EQ(Read(*adapter, "connectCount").integer, 2);
        EXPECT_EQ(Read(*adapter, "awakeCount").integer, 1); EXPECT_EQ(Read(*adapter, "startCount").integer, 1);
    }
}

TEST_F(ManagedCompletionTest, StructuredSettingsRemapOnlyTypedPrefabReferences)
{
    const std::string source = "source-object";
    const std::string instance = "instance-object";
    scene::managed_detail::StructuredValue value{R"({"$version":1,"value":{"target":{"$go":"source-object"},"text":"source-object","tokenText":"{\"$go\":\"source-object\"}"}})"};
    ASSERT_TRUE(value.Valid());
    toml::table table;
    util::TomlWriteReflector writer(table);
    writer.BeginField("settings", "Settings");
    writer.ObjectField("settings", value);
    writer.EndField();
    ASSERT_NE(table["settings"]["json"].as_string(), nullptr);
    ASSERT_NE(table["settings"]["objectGuids"].as_array(), nullptr);
    scene::RemapPrefabObjectReferences(table, {{source, instance}});
    scene::ManagedFieldValue saved;
    saved.value.valueKind = scene::ManagedValueKind::STRUCTURED;
    ASSERT_TRUE(scene::managed_detail::ReadSavedField(*table.get("settings"), saved));
    EXPECT_NE(saved.text.find(R"("$go":"instance-object")"), std::string::npos);
    EXPECT_NE(saved.text.find(R"("text":"source-object")"), std::string::npos);
    EXPECT_NE(saved.text.find(R"("tokenText":"{\"$go\":\"source-object\"}")"), std::string::npos);
}

TEST_F(ManagedCompletionTest, FailedConnectNeverPublishesAndFaultedOwnerNeverReconnects)
{
    auto& publisherObject = m_scene.CreateGameObject("Publisher");
    auto& failedObject = m_scene.CreateGameObject("Failed");
    auto* publisher = Add(publisherObject, "cs-internal:ManagedFixtures.CompletionComponent");
    auto* failed = Add(failedObject, "cs-internal:ManagedFixtures.CompletionComponent");
    ASSERT_NE(publisher, nullptr); ASSERT_NE(failed, nullptr);
    ASSERT_TRUE(failed->ApplySettingsJson(R"({"failConnect":true})"));
    Tick();
    ASSERT_TRUE(failed->IsRuntimeFaulted());
    ASSERT_TRUE(publisher->ApplySettingsJson(R"({"command":3})"));
    Tick();
    EXPECT_EQ(Read(*publisher, "eventSum").integer, 3);
    EXPECT_EQ(Read(*failed, "eventSum").integer, 0);
    EXPECT_EQ(Read(*failed, "connectCount").integer, 1);
    failed->enabled = true;
    Tick();
    EXPECT_TRUE(failed->IsRuntimeFaulted()); EXPECT_TRUE(failed->enabled);
    EXPECT_EQ(Read(*failed, "connectCount").integer, 1);
    ASSERT_TRUE(publisher->ApplySettingsJson(R"({"command":3})"));
    Tick();
    EXPECT_EQ(Read(*publisher, "eventSum").integer, 6);
    EXPECT_EQ(Read(*failed, "eventSum").integer, 0);
}

TEST_F(ManagedCompletionTest, ExplicitResumeRetriesFailedStagesAndRevalidatesRequiredSettings)
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    for (int phase = 1; phase <= 5; ++phase) {
        auto& object = m_scene.CreateGameObject("Recovery" + std::to_string(phase));
        auto* adapter = Add(object, "cs-internal:ManagedFixtures.FaultRecoveryComponent");
        ASSERT_NE(adapter, nullptr);
        const std::string target = "\"target\":\"" + object.instanceId + "\"";
        ASSERT_TRUE(adapter->ApplySettingsJson("{" + target + ",\"failPhase\":" + std::to_string(phase) + "}"));
        Tick();
        ASSERT_TRUE(adapter->IsRuntimeFaulted()) << phase;
        ASSERT_FALSE(adapter->enabled);
        const auto diagnostic = adapter->FaultDiagnostic();
        ASSERT_NE(diagnostic.find("FaultRecovery phase " + std::to_string(phase)), std::string::npos);
        ASSERT_TRUE(adapter->ApplySettingsJson(R"({"failPhase":0,"target":null})",
            scene::ManagedScriptAdapter::SettingsValidationPurpose::AUTHORING));
        EXPECT_FALSE(runtime.Resume(*adapter));
        EXPECT_TRUE(adapter->IsRuntimeFaulted()); EXPECT_FALSE(adapter->enabled);
        EXPECT_EQ(adapter->FaultDiagnostic(), diagnostic);
        EXPECT_FALSE(adapter->ResumeDiagnostic().empty());
        ASSERT_TRUE(adapter->ApplySettingsJson("{" + target + "}"));
        const auto awakeBefore = Read(*adapter, "awakeCount").integer;
        const auto startBefore = Read(*adapter, "startCount").integer;
        ASSERT_TRUE(runtime.Resume(*adapter)) << runtime.LastDiagnostic();
        EXPECT_FALSE(adapter->IsRuntimeFaulted()); EXPECT_TRUE(adapter->FaultDiagnostic().empty());
        EXPECT_TRUE(adapter->ResumeDiagnostic().empty());
        EXPECT_EQ(Read(*adapter, "awakeCount").integer, awakeBefore);
        EXPECT_EQ(Read(*adapter, "startCount").integer, startBefore);
        Tick();
        EXPECT_FALSE(adapter->IsRuntimeFaulted()) << adapter->FaultDiagnostic();
        EXPECT_EQ(Read(*adapter, "awakeCount").integer, phase == 1 ? 2 : 1) << phase;
        EXPECT_EQ(Read(*adapter, "startCount").integer, phase == 2 ? 2 : 1) << phase;
        EXPECT_EQ(Read(*adapter, "connectCount").integer, phase == 3 || phase == 4 ? 2 : 1) << phase;
        EXPECT_FALSE(runtime.Resume(*adapter)) << "A running instance must not be reset by resume.";
    }
}

TEST_F(ManagedCompletionTest, ResumeClearsRequiredBlockWithoutRepeatingSuccessfulAwake)
{
    auto& object = m_scene.CreateGameObject("Blocked");
    auto* adapter = Add(object, "cs-internal:ManagedFixtures.FaultRecoveryComponent");
    ASSERT_NE(adapter, nullptr);
    Tick();
    ASSERT_TRUE(adapter->RequirementsBlocked()); ASSERT_FALSE(adapter->IsRuntimeFaulted());
    EXPECT_EQ(Read(*adapter, "awakeCount").integer, 1);
    EXPECT_EQ(Read(*adapter, "startCount").integer, 0);
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    EXPECT_FALSE(runtime.Resume(*adapter)); EXPECT_TRUE(adapter->RequirementsBlocked());
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"target\":\"" + object.instanceId + "\"}"));
    ASSERT_TRUE(runtime.Resume(*adapter)) << runtime.LastDiagnostic();
    Tick();
    EXPECT_FALSE(adapter->RequirementsBlocked()); EXPECT_FALSE(adapter->IsRuntimeFaulted());
    EXPECT_EQ(Read(*adapter, "awakeCount").integer, 1);
    EXPECT_EQ(Read(*adapter, "startCount").integer, 1);
    EXPECT_EQ(Read(*adapter, "connectCount").integer, 1);
}

TEST_F(ManagedCompletionTest, ResumeRevalidatesAfterCancellingARoutineThatChangesRequiredSettings)
{
    auto& object = m_scene.CreateGameObject("DisposeRecovery");
    auto* adapter = Add(object, "cs-internal:ManagedFixtures.FaultRecoveryComponent");
    ASSERT_NE(adapter, nullptr);
    const std::string target = "{\"target\":\"" + object.instanceId + "\"}";
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"target\":\"" + object.instanceId + "\",\"clearRequiredOnRoutineDispose\":true}"));
    Tick(); ASSERT_TRUE(adapter->RequirementsBlocked());
    EXPECT_EQ(Read(*adapter, "routineStartCount").integer, 0);
    EXPECT_EQ(Read(*adapter, "routineDisposeCount").integer, 0);
    ASSERT_TRUE(adapter->ApplySettingsJson(target));
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    EXPECT_FALSE(runtime.Resume(*adapter));
    EXPECT_TRUE(adapter->RequirementsBlocked()); EXPECT_FALSE(adapter->enabled);
    EXPECT_EQ(Read(*adapter, "routineStartCount").integer, 0);
    EXPECT_EQ(Read(*adapter, "routineDisposeCount").integer, 1);
    ASSERT_TRUE(adapter->ApplySettingsJson(target));
    ASSERT_TRUE(runtime.Resume(*adapter)) << runtime.LastDiagnostic(); Tick();
    EXPECT_FALSE(adapter->RequirementsBlocked()); EXPECT_FALSE(adapter->IsRuntimeFaulted());
    EXPECT_EQ(Read(*adapter, "awakeCount").integer, 1); EXPECT_EQ(Read(*adapter, "startCount").integer, 1);
    EXPECT_EQ(Read(*adapter, "routineDisposeCount").integer, 1);
}

TEST_F(ManagedCompletionTest, DisabledResumeKeepsCancelledJobsAndReconnectsExactlyOnceWhenEnabled)
{
    auto& object = m_scene.CreateGameObject("Recovery");
    auto& publisherObject = m_scene.CreateGameObject("Publisher");
    auto* adapter = Add(object, "cs-internal:ManagedFixtures.FaultRecoveryComponent");
    auto* publisher = Add(publisherObject, "cs-internal:ManagedFixtures.CompletionComponent");
    ASSERT_NE(adapter, nullptr); ASSERT_NE(publisher, nullptr);
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"target\":\"" + object.instanceId + "\"}"));
    Tick();
    ASSERT_TRUE(adapter->ApplySettingsJson(R"({"failPhase":4,"command":2})"));
    Tick();
    ASSERT_TRUE(adapter->IsRuntimeFaulted());
    ASSERT_TRUE(publisher->ApplySettingsJson(R"({"command":3})")); Tick();
    EXPECT_EQ(Read(*adapter, "eventCount").integer, 0);
    ASSERT_TRUE(adapter->ApplySettingsJson(R"({"failPhase":0})"));
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Resume(*adapter, false)) << runtime.LastDiagnostic();
    for (int frame = 0; frame < 4; ++frame) Tick();
    EXPECT_FALSE(adapter->enabled); EXPECT_EQ(Read(*adapter, "jobCount").integer, 0);
    EXPECT_EQ(Read(*adapter, "awakeCount").integer, 1); EXPECT_EQ(Read(*adapter, "startCount").integer, 1);
    EXPECT_EQ(Read(*adapter, "connectCount").integer, 1);
    adapter->enabled = true; Tick(); Tick();
    EXPECT_EQ(Read(*adapter, "connectCount").integer, 2);
    ASSERT_TRUE(publisher->ApplySettingsJson(R"({"command":3})")); Tick();
    EXPECT_EQ(Read(*adapter, "eventCount").integer, 3);
    EXPECT_EQ(Read(*adapter, "jobCount").integer, 0);
}

TEST_F(ManagedCompletionTest, ResumeDefersReconnectWhileTheGameObjectIsInactive)
{
    auto& object = m_scene.CreateGameObject("InactiveRecovery");
    auto* adapter = Add(object, "cs-internal:ManagedFixtures.FaultRecoveryComponent");
    ASSERT_NE(adapter, nullptr);
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"target\":\"" + object.instanceId + "\",\"failPhase\":4}")); Tick();
    ASSERT_TRUE(adapter->IsRuntimeFaulted());
    ASSERT_TRUE(adapter->ApplySettingsJson(R"({"failPhase":0})"));
    object.SetActive(false);
    ASSERT_TRUE(scene::ManagedScriptRuntime::Instance().Resume(*adapter));
    Tick(); Tick();
    EXPECT_TRUE(adapter->enabled); EXPECT_FALSE(adapter->IsRuntimeFaulted());
    EXPECT_EQ(Read(*adapter, "connectCount").integer, 1); EXPECT_EQ(Read(*adapter, "updateCount").integer, 1);
    object.SetActive(true); Tick(); Tick();
    EXPECT_EQ(Read(*adapter, "connectCount").integer, 2);
    EXPECT_EQ(Read(*adapter, "awakeCount").integer, 1); EXPECT_EQ(Read(*adapter, "startCount").integer, 1);
}

TEST_F(ManagedCompletionTest, ResumeAlsoClearsOwnedJobAndEventFailuresWithoutReplayingTheirWork)
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    for (int phase : {6, 7}) {
        auto& object = m_scene.CreateGameObject("OwnedRecovery" + std::to_string(phase));
        auto* adapter = Add(object, "cs-internal:ManagedFixtures.FaultRecoveryComponent");
        ASSERT_NE(adapter, nullptr);
        ASSERT_TRUE(adapter->ApplySettingsJson("{\"target\":\"" + object.instanceId + "\"}")); Tick();
        ASSERT_TRUE(adapter->ApplySettingsJson("{\"failPhase\":" + std::to_string(phase)
            + ",\"command\":" + std::to_string(phase == 6 ? 2 : 1) + "}"));
        Tick();
        if (phase == 6) { Tick(); Tick(); }
        ASSERT_TRUE(adapter->IsRuntimeFaulted()) << adapter->FaultDiagnostic();
        const auto jobs = Read(*adapter, "jobCount").integer;
        const auto events = Read(*adapter, "eventCount").integer;
        ASSERT_TRUE(adapter->ApplySettingsJson(R"({"failPhase":0})"));
        ASSERT_TRUE(runtime.Resume(*adapter)) << runtime.LastDiagnostic();
        for (int frame = 0; frame < 3; ++frame) Tick();
        EXPECT_FALSE(adapter->IsRuntimeFaulted()) << adapter->FaultDiagnostic();
        EXPECT_EQ(Read(*adapter, "awakeCount").integer, 1); EXPECT_EQ(Read(*adapter, "startCount").integer, 1);
        EXPECT_EQ(Read(*adapter, "connectCount").integer, 2);
        EXPECT_EQ(Read(*adapter, "jobCount").integer, jobs);
        EXPECT_EQ(Read(*adapter, "eventCount").integer, events);
    }
}

TEST_F(ManagedCompletionTest, InvalidUtf16ExceptionMessageKeepsTheOriginalFailureAndDoesNotReplayTheCallback)
{
    auto& object = m_scene.CreateGameObject("InvalidMessage");
    auto* adapter = Add(object, "cs-internal:ManagedFixtures.FaultRecoveryComponent");
    ASSERT_NE(adapter, nullptr);
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"target\":\"" + object.instanceId + "\",\"failPhase\":4,\"invalidMessage\":true}"));
    Tick();
    ASSERT_TRUE(adapter->IsRuntimeFaulted()); EXPECT_FALSE(adapter->enabled);
    const auto diagnostic = adapter->FaultDiagnostic();
    EXPECT_TRUE(diagnostic.starts_with("System.InvalidOperationException: FaultRecovery phase 4:"));
    EXPECT_NE(diagnostic.find("invalid \xEF\xBF\xBD message"), std::string::npos);
    EXPECT_EQ(diagnostic.find("EncoderFallbackException"), std::string::npos);
    EXPECT_EQ(Read(*adapter, "updateCount").integer, 1);
    Tick();
    EXPECT_TRUE(adapter->IsRuntimeFaulted()); EXPECT_EQ(adapter->FaultDiagnostic(), diagnostic);
    EXPECT_EQ(Read(*adapter, "updateCount").integer, 1);
}

TEST_F(ManagedCompletionTest, ReloadKeepsFaultLatchedUntilExplicitResumeAndLongDiagnosticKeepsItsCause)
{
    auto& object = m_scene.CreateGameObject("LongFailure");
    auto* adapter = Add(object, "cs-internal:ManagedFixtures.FaultRecoveryComponent");
    ASSERT_NE(adapter, nullptr);
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"target\":\"" + object.instanceId + "\",\"failPhase\":4,\"longMessage\":true}"));
    Tick();
    ASSERT_TRUE(adapter->IsRuntimeFaulted());
    const auto diagnostic = adapter->FaultDiagnostic();
    EXPECT_TRUE(diagnostic.starts_with("System.InvalidOperationException: FaultRecovery phase 4:"));
    EXPECT_TRUE(diagnostic.ends_with("[diagnostic truncated]"));
    EXPECT_LE(diagnostic.size(), 8192U);
    EXPECT_EQ(Read(*adapter, "updateCount").integer, 1);
    ASSERT_TRUE(adapter->ApplySettingsJson(R"({"failPhase":0,"longMessage":false})"));
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Reload(m_scene, FBZZ_MANAGED_FIXTURE_V2_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    EXPECT_TRUE(adapter->IsRuntimeFaulted()); EXPECT_FALSE(adapter->enabled);
    EXPECT_EQ(adapter->FaultDiagnostic(), diagnostic);
    Tick(); EXPECT_EQ(Read(*adapter, "updateCount").integer, 1);
    ASSERT_TRUE(runtime.Resume(*adapter)) << runtime.LastDiagnostic();
    Tick();
    EXPECT_FALSE(adapter->IsRuntimeFaulted());
    EXPECT_EQ(Read(*adapter, "awakeCount").integer, 1); EXPECT_EQ(Read(*adapter, "startCount").integer, 1);
    EXPECT_EQ(Read(*adapter, "updateCount").integer, 2);
    EXPECT_EQ(Read(*adapter, "connectCount").integer, 2);
}

TEST_F(ManagedCompletionTest, LoadsDataAssetAndRoundTripsStructuredSettingsThroughTheRealHost)
{
    const auto path = m_temp.Path() / "weapon.fzdata";
    ASSERT_TRUE(util::FileSystem::WriteText(path, "type = 'ManagedFixtures.WeaponData'\ndamage = 12\nrotation = [0.0, 0.0, 0.0, 1.0]\nweights = [4.0, 5.0]\n"));
    auto& object = m_scene.CreateGameObject("Owner");
    auto* adapter = Add(object, "cs-internal:ManagedFixtures.CompletionComponent");
    ASSERT_NE(adapter, nullptr);
    auto& target = m_scene.CreateGameObject("SettingsTarget");
    const std::string json = "{\"weapon\":\"" + path.generic_string()
        + "\",\"settings\":{\"label\":\"saved\",\"weights\":[3,4],\"target\":\"" + target.instanceId
        + "\"},\"rotation\":[0,0,0.5,0.5]}";
    ASSERT_TRUE(adapter->ApplySettingsJson(json));
    Tick();
    ASSERT_TRUE(adapter->ApplySettingsJson(R"({"command":5})"));
    Tick();
    EXPECT_EQ(Read(*adapter, "assetDamage").integer, 12);
    EXPECT_FALSE(adapter->IsRuntimeFaulted()) << adapter->FaultDiagnostic();
    const std::string settings = ReadText(*adapter, "settings");
    const std::string serialized = scene::SceneSerializer::SaveToText(m_scene);
    ASSERT_FALSE(serialized.empty());
    auto restored = scene::SceneSerializer::LoadDataFromText(serialized, {});
    ASSERT_NE(restored, nullptr);
    auto* restoredOwner = restored->Find("Owner");
    ASSERT_NE(restoredOwner, nullptr);
    auto* component = restoredOwner->GetComponent<scene::ScriptComponent>();
    ASSERT_NE(component, nullptr); ASSERT_EQ(component->scripts.size(), 1);
    auto* restoredAdapter = scene::ManagedScriptRuntime::Instance().FindAdapter(component->scripts.front().script.get());
    ASSERT_NE(restoredAdapter, nullptr);
    EXPECT_EQ(ReadText(*restoredAdapter, "settings"), settings);
    EXPECT_EQ(ReadText(*restoredAdapter, "weapon"), path.generic_string());
    const auto rotation = Read(*restoredAdapter, "rotation");
    EXPECT_NEAR(rotation.z, 0.5f, testkit::kTolerance); EXPECT_NEAR(rotation.w, 0.5f, testkit::kTolerance);
    auto* restoredTarget = restored->Find("SettingsTarget");
    ASSERT_NE(restoredTarget, nullptr);
    ASSERT_TRUE(restoredAdapter->ApplySettingsJson(R"({"command":6})"));
    SystemContext context{*restored, m_world, nullptr, nullptr, 0.25f, 0.02f, true, true};
    scene::ScriptSystem{}.Update(context);
    EXPECT_FALSE(restoredAdapter->IsRuntimeFaulted()) << restoredAdapter->FaultDiagnostic();
    EXPECT_EQ(Read(*restoredAdapter, "cacheChecks").integer, 1);
    EXPECT_EQ(Read(*restoredAdapter, "cacheFailures").integer, 0);
    EXPECT_NEAR(restoredTarget->transform.position.x, 1, testkit::kTolerance);
}

TEST_F(ManagedCompletionTest, ConnectionUsesSharedCallbackPolicyAndRejectsWorldMutations)
{
    EXPECT_STREQ(scene::ManagedCallbackPolicy(scene::ManagedCallback::CONNECT), "subscription_only");
    EXPECT_STREQ(scene::ManagedCallbackPolicy(scene::ManagedCallback::DESTROY), "no_engine_api");
    EXPECT_STREQ(scene::ManagedCallbackPolicy(scene::ManagedCallback::JOB), "live");
    EXPECT_STREQ(scene::ManagedCallbackPolicy(static_cast<scene::ManagedCallback>(99)), "no_engine_api");
    scene::Script::SetInPlayMode(true);
    auto& owner = m_scene.CreateGameObject("ConnectionOwner");
    auto* adapter = Add(owner, "cs-internal:ManagedFixtures.ConnectionPolicyComponent");
    ASSERT_NE(adapter, nullptr);
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Invoke(*adapter, scene::ManagedCallback::CONNECT)) << runtime.LastDiagnostic();
    EXPECT_NE(Read(*adapter, "ManagedFixtures.ConnectionPolicyComponent.m_readSucceeded").integer, 0);
    ASSERT_TRUE(adapter->ApplySettingsJson(R"({"mutate":true})"));
    EXPECT_FALSE(runtime.Invoke(*adapter, scene::ManagedCallback::CONNECT));
    EXPECT_EQ(m_scene.Find("ForbiddenConnectMutation"), nullptr);
    EXPECT_FALSE(runtime.LastDiagnostic().empty());
}

TEST_F(ManagedCompletionTest, TypedRequirementsReportMissingTypesWithoutAddingAndUseAssignableSameOwnerScripts)
{
    auto& owner = m_scene.CreateGameObject("TypedRequirements");
    auto* dependent = Add(owner, "cs-internal:ManagedFixtures.RequiredAuthoringComponent");
    ASSERT_NE(dependent, nullptr);
    EXPECT_EQ(dependent->ExecutionOrder(), -20);
    ASSERT_EQ(dependent->RequiredComponents().size(), 1u);
    EXPECT_EQ(dependent->RequiredComponents()[0], "CameraComponent");
    std::vector<scene::ScriptRequirementIssue> issues;
    scene::CollectScriptRequirementIssues(owner, *dependent, issues);
    ASSERT_FALSE(issues.empty());
    EXPECT_EQ(owner.GetComponent<scene::CameraComponent>(), nullptr);
    EXPECT_FALSE(dependent->RequirementsBlocked());
    owner.AddComponent<scene::CameraComponent>();
    issues.clear();
    scene::CollectScriptRequirementIssues(owner, *dependent, issues);
    ASSERT_FALSE(issues.empty());
    EXPECT_NE(scene::FormatScriptRequirementIssue(issues.back()).find("RequiredPeerBase"), std::string::npos);
    auto& elsewhere = m_scene.CreateGameObject("OtherOwner");
    ASSERT_NE(Add(elsewhere, "cs-internal:ManagedFixtures.RequiredPeerComponent"), nullptr);
    EXPECT_FALSE(dependent->ValidateReferences());
    auto* peer = Add(owner, "cs-internal:ManagedFixtures.RequiredPeerComponent");
    ASSERT_NE(peer, nullptr);
    peer->enabled = false;
    EXPECT_TRUE(dependent->ValidateReferences()) << scene::ManagedScriptRuntime::Instance().LastDiagnostic();
    Tick();
    EXPECT_EQ(Read(*dependent, "starts").integer, 1);
    EXPECT_EQ(Read(*dependent, "updates").integer, 1);
}

TEST_F(ManagedCompletionTest, EarlierRemovalStopsRequiredDependentInSameFrameForScriptAndNativeRows)
{
    for (const int command : {1, 2}) {
        auto& owner = m_scene.CreateGameObject("Removal" + std::to_string(command));
        owner.AddComponent<scene::CameraComponent>();
        auto* dependent = Add(owner, "cs-internal:ManagedFixtures.RequiredAuthoringComponent");
        auto* peer = Add(owner, "cs-internal:ManagedFixtures.RequiredPeerComponent");
        auto* remover = Add(owner, "cs-internal:ManagedFixtures.RequirementRemovalComponent");
        ASSERT_NE(dependent, nullptr); ASSERT_NE(peer, nullptr); ASSERT_NE(remover, nullptr);
        Tick();
        const auto before = Read(*dependent, "updates").integer;
        ASSERT_TRUE(remover->ApplySettingsJson("{\"command\":" + std::to_string(command) + "}"));
        Tick();
        EXPECT_EQ(Read(*dependent, "updates").integer, before);
        EXPECT_TRUE(dependent->RequirementsBlocked());
        EXPECT_FALSE(dependent->IsRuntimeFaulted());
        EXPECT_NE(dependent->FaultDiagnostic().find(command == 1 ? "RequiredPeerBase" : "Camera"), std::string::npos);
    }
}

TEST_F(ManagedCompletionTest, ManagedExecutionOrderMetadataControlsNativeSchedulerAndSurvivesReload)
{
    auto& owner = m_scene.CreateGameObject("ManagedOrdered");
    auto* late = Add(owner, "cs-internal:ManagedFixtures.OrderedLateComponent");
    auto* early = Add(owner, "cs-internal:ManagedFixtures.OrderedEarlyComponent");
    ASSERT_NE(late, nullptr); ASSERT_NE(early, nullptr);
    Tick();
    EXPECT_NEAR(owner.transform.position.x, 10.0f, testkit::kTolerance);
    ASSERT_TRUE(scene::ManagedScriptRuntime::Instance().Reload(m_scene, FBZZ_MANAGED_FIXTURE_V2_TEST_ASSEMBLY, true));
    EXPECT_EQ(early->ExecutionOrder(), -10);
    EXPECT_EQ(late->ExecutionOrder(), 10);
    Tick();
    EXPECT_NEAR(owner.transform.position.x, 110.0f, testkit::kTolerance);
}

TEST_F(ManagedCompletionTest, MissingTypedPeerBlocksAwakeWithoutFaultingUserCode)
{
    auto& owner = m_scene.CreateGameObject("MissingPeerBeforeAwake");
    owner.AddComponent<scene::CameraComponent>();
    auto* dependent = Add(owner, "cs-internal:ManagedFixtures.RequiredAuthoringComponent");
    ASSERT_NE(dependent, nullptr);
    Tick();
    EXPECT_EQ(Read(*dependent, "awakes").integer, 0);
    EXPECT_EQ(Read(*dependent, "starts").integer, 0);
    EXPECT_EQ(Read(*dependent, "updates").integer, 0);
    EXPECT_TRUE(dependent->RequirementsBlocked());
    EXPECT_FALSE(dependent->IsRuntimeFaulted());
    EXPECT_NE(dependent->FaultDiagnostic().find("RequiredPeerBase"), std::string::npos);
}

namespace {
/// @note A synchronous nested scheduler call must not borrow the outer scheduler's entity or identity storage.
class SnapshotReentryScript final : public scene::Script {
public:
    SnapshotReentryScript(std::vector<int>& record, int tag, int order, std::function<void()> nested)
        : m_record(record), m_tag(tag), m_order(order), m_nested(std::move(nested)) {}
    int ExecutionOrder() const override { return m_order; }
    void OnUpdate() override { m_record.push_back(m_tag); if (m_nested) m_nested(); }
    void OnFixedUpdate() override { m_record.push_back(m_tag + 1000); }
    void OnLateUpdate() override { m_record.push_back(m_tag + 2000); }
private:
    std::vector<int>& m_record;
    int m_tag, m_order;
    std::function<void()> m_nested;
};
class ScriptSnapshotWorkspaceTest : public testkit::EngineFixture {
protected:
    void SetUp() override { EngineFixture::SetUp(); m_play = scene::Script::IsInPlayMode(); }
    void TearDown() override { scene::Script::SetInPlayMode(m_play); EngineFixture::TearDown(); }
    bool m_play = false;
};
} /// @note namespace

TEST_F(ScriptSnapshotWorkspaceTest, NestedAndSuccessiveScenesKeepIndependentIdentityAndExecutionOrder)
{
    std::vector<int> record;
    scene::Scene outer, nested;
    physics::World world;
    SystemContext context{outer, world, nullptr, nullptr, .02f, .02f, true, true};
    SystemContext nestedContext{nested, world, nullptr, nullptr, .02f, .02f, true, true};
    const auto append = [&](scene::Scene& target, int tag, int order, std::function<void()> callback = {}) {
        auto& owner = target.CreateGameObject("SnapshotOwner" + std::to_string(tag));
        auto& entry = owner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
        entry.inspectionId = std::string(2048 + tag, 'x') + std::to_string(tag);
        entry.script = std::make_unique<SnapshotReentryScript>(record, tag, order, std::move(callback));
    };
    for (int tag = 0; tag < 128; ++tag) append(nested, tag + 100, tag);
    bool entered = false;
    append(outer, 2, 2);
    append(outer, 0, 0, [&] { if (!entered) { entered = true; scene::ScriptSystem{}.Update(nestedContext); } });
    append(outer, 1, 1);
    scene::ScriptSystem{}.Update(context);
    std::vector<int> expected{0};
    for (int tag = 0; tag < 128; ++tag) expected.push_back(tag + 100);
    expected.insert(expected.end(), {1,2});
    EXPECT_EQ(record, expected);
    record.clear();
    scene::ScriptSystem{}.Update(context);
    scene::FixedScriptSystem{}.Update(context);
    scene::LateScriptSystem{}.Update(context);
    EXPECT_EQ(record, (std::vector<int>{0,1,2,1000,1001,1002,2000,2001,2002}));
    record.clear();
    outer.Clear(); nested.Clear();
    append(outer, 9, 0);
    scene::ScriptSystem{}.Update(context);
    EXPECT_EQ(record, (std::vector<int>{9}));
}

TEST_F(ScriptSnapshotWorkspaceTest, DelayedDisableStopsTheTurnAndLaterReenableRunsPendingWorkOnTheNextTurn)
{
    std::vector<int> record;
    scene::Scene target;
    physics::World world;
    SystemContext context{target, world, nullptr, nullptr, .02f, .02f, true, true};
    auto& first = target.CreateGameObject("DelayedDisable");
    auto blocked = std::make_unique<SnapshotReentryScript>(record, 3, 0, std::function<void()>{});
    auto* script = blocked.get();
    script->FrameDelay(0, [&] { record.push_back(3000); script->enabled = false; });
    const auto invocation = script->Invoke([&] { record.push_back(3001); }, 0);
    ASSERT_TRUE(invocation.IsValid());
    first.AddComponent<scene::ScriptComponent>().scripts.emplace_back().script = std::move(blocked);
    auto& later = target.CreateGameObject("LaterEnable");
    later.AddComponent<scene::ScriptComponent>().scripts.emplace_back().script =
        std::make_unique<SnapshotReentryScript>(record, 4, 1, [&] { script->enabled = true; });
    scene::ScriptSystem{}.Update(context);
    EXPECT_EQ(record, (std::vector<int>{3000,4}));
    record.clear();
    scene::ScriptSystem{}.Update(context);
    EXPECT_EQ(record, (std::vector<int>{3001,3,4}));
}

TEST_F(ManagedCompletionTest, ScriptSnapshotPreservesLongDistinctEntryIdsAcrossArenaGrowth)
{
    scene::Script::SetInPlayMode(true);
    auto& owner = m_scene.CreateGameObject("SnapshotIdentityArena");
    auto* first = Add(owner, "cs-internal:ManagedFixtures.LifecycleComponent");
    auto* second = Add(owner, "cs-internal:ManagedFixtures.LifecycleComponent");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(second->ApplySettingsJson(R"({"speed":8})"));
    auto* scripts = owner.GetComponent<scene::ScriptComponent>();
    ASSERT_NE(scripts, nullptr);
    ASSERT_EQ(scripts->scripts.size(), 2u);
    const std::string prefix(1024, 'x');
    scripts->scripts[0].inspectionId = prefix + "first";
    scripts->scripts[1].inspectionId = prefix + "second";
    Tick();
    Tick();
    EXPECT_FALSE(first->IsRuntimeFaulted());
    EXPECT_FALSE(second->IsRuntimeFaulted());
    for (auto* adapter : {first, second}) {
        EXPECT_EQ(Read(*adapter, "awakeCount").integer, 1);
        EXPECT_EQ(Read(*adapter, "startCount").integer, 1);
        EXPECT_EQ(Read(*adapter, "updateCount").integer, 2);
    }
    EXPECT_VEC3_NEAR(owner.transform.position, (math::Vector3{6, 0, 0}), testkit::kTolerance);
}

TEST_F(ManagedCompletionTest, CachedOwnerBindingRestoresAfterDirectHostUnbinding)
{
    scene::Script::SetInPlayMode(true);
    auto& owner = m_scene.CreateGameObject("CachedBindingOwner");
    auto* adapter = Add(owner, "cs-internal:ManagedFixtures.LifecycleComponent");
    ASSERT_NE(adapter, nullptr);
    ASSERT_TRUE(adapter->RefreshOwnerBinding());
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    scene::ManagedBridgeRequest request;
    request.op = scene::ManagedBridgeOp::BIND_OWNER;
    request.generation = adapter->Generation();
    request.instanceId = adapter->InstanceId();
    request.ownerId = adapter->OwnerId();
    const std::string unbound = "\n" + adapter->ReferenceBirth();
    request.input = unbound.data();
    request.inputBytes = static_cast<uint32_t>(unbound.size());
    scene::ManagedBridgeReply reply;
    ASSERT_TRUE(runtime.Dispatch(request, reply)) << runtime.LastDiagnostic();

    ASSERT_TRUE(adapter->RefreshOwnerBinding()) << runtime.LastDiagnostic();
    adapter->OnUpdate();
    EXPECT_FALSE(adapter->IsRuntimeFaulted()) << adapter->FaultDiagnostic();
    EXPECT_EQ(Read(*adapter, "updateCount").integer, 1);
    EXPECT_VEC3_NEAR(owner.transform.position, (math::Vector3{1, 0, 0}), testkit::kTolerance);
}

TEST_F(ManagedCompletionTest, CachedOwnerBindingRejectsDirectReleaseWithoutOwnerId)
{
    auto& owner = m_scene.CreateGameObject("ReleasedCachedBinding");
    auto* adapter = Add(owner, "cs-internal:ManagedFixtures.HealthComponent");
    ASSERT_NE(adapter, nullptr);
    ASSERT_TRUE(adapter->RefreshOwnerBinding());
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    scene::ManagedBridgeRequest request;
    request.op = scene::ManagedBridgeOp::RELEASE_INSTANCE;
    request.generation = adapter->Generation();
    request.instanceId = adapter->InstanceId();
    scene::ManagedBridgeReply reply;
    ASSERT_TRUE(runtime.Dispatch(request, reply)) << runtime.LastDiagnostic();
    EXPECT_FALSE(adapter->RefreshOwnerBinding());
    EXPECT_FALSE(adapter->RefreshOwnerBinding());
}

TEST_F(ManagedCompletionTest, CachedOwnerBindingRetargetsAnExistingScriptEntry)
{
    scene::Script::SetInPlayMode(true);
    auto& first = m_scene.CreateGameObject("FirstBindingOwner");
    auto& second = m_scene.CreateGameObject("SecondBindingOwner");
    auto* adapter = Add(first, "cs-internal:ManagedFixtures.LifecycleComponent");
    ASSERT_NE(adapter, nullptr);
    ASSERT_TRUE(adapter->RefreshOwnerBinding());
    auto& entry = second.AddComponent<scene::ScriptComponent>().scripts.emplace_back(
        std::move(first.GetComponent<scene::ScriptComponent>()->scripts.front()));
    ASSERT_EQ(entry.script.get(), adapter);
    adapter->SetContext(&m_scene, &second);
    ASSERT_TRUE(adapter->RefreshOwnerBinding());
    adapter->OnUpdate();
    EXPECT_FALSE(adapter->IsRuntimeFaulted()) << adapter->FaultDiagnostic();
    EXPECT_VEC3_NEAR(first.transform.position, math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_VEC3_NEAR(second.transform.position, (math::Vector3{1, 0, 0}), testkit::kTolerance);
}

TEST_F(ManagedCompletionTest, CachedOwnerBindingRejectsAnotherSceneInsideManagedCallback)
{
    auto& publisher = m_scene.CreateGameObject("BindingScopePublisher");
    auto& owner = m_scene.CreateGameObject("BindingScopeListener");
    auto* caller = Add(publisher, "cs-internal:ManagedFixtures.CompletionComponent");
    auto* listener = Add(owner, "cs-internal:ManagedFixtures.HealthComponent");
    ASSERT_NE(caller, nullptr);
    ASSERT_NE(listener, nullptr);
    Tick();
    ASSERT_EQ(Read(*caller, "connectCount").integer, 1);
    scene::Scene other;
    auto& foreignOwner = other.CreateGameObject("ForeignBindingOwner");
    auto& entry = foreignOwner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.HealthComponent", entry.savedScriptId);
    ASSERT_NE(entry.script, nullptr);
    entry.script->SetContext(&other, &foreignOwner);
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    auto* foreign = runtime.FindAdapter(entry.script.get());
    ASSERT_NE(foreign, nullptr);
    ASSERT_TRUE(foreign->RefreshOwnerBinding());
    ASSERT_TRUE(foreign->RefreshOwnerBinding());
    int deliveries = 0;
    bool nestedSucceeded = true;
    const auto subscription = scene::ScriptEventBus::SubscribeRaw(listener, "managed:System.Int32", [&](const void*) {
        ++deliveries;
        nestedSucceeded = foreign->RefreshOwnerBinding();
    });
    ASSERT_TRUE(subscription.IsValid());
    EXPECT_TRUE(caller->ApplySettingsJson(R"({"command":3})"));
    Tick();
    scene::ScriptEventBus::Unsubscribe(subscription);
    EXPECT_EQ(deliveries, 1);
    EXPECT_FALSE(nestedSucceeded);
    EXPECT_EQ(Read(*caller, "eventSum").integer, 3);
    EXPECT_FALSE(caller->IsRuntimeFaulted());
    EXPECT_TRUE(foreign->RefreshOwnerBinding());
    other.Clear();
}

TEST_F(ManagedCompletionTest, EmptyGizmoCallbacksPreserveCapacityContextAndThreadRejection)
{
    auto& owner = m_scene.CreateGameObject("NoGizmoCallbacks");
    auto* adapter = Add(owner, "cs-internal:ManagedFixtures.HealthComponent");
    ASSERT_NE(adapter, nullptr);
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    std::vector<scene::ScriptDebugDrawCommand> commands(1);
    ASSERT_TRUE(runtime.CaptureGizmos(*adapter, false, commands));
    EXPECT_TRUE(commands.empty());
    commands.emplace_back();
    ASSERT_TRUE(runtime.CaptureGizmos(*adapter, true, commands));
    EXPECT_TRUE(commands.empty());

    for (const uint32_t capacity : {0U, (std::numeric_limits<uint32_t>::max)()}) {
        commands.emplace_back();
        EXPECT_FALSE(runtime.CaptureGizmos(*adapter, false, commands, capacity));
        EXPECT_TRUE(commands.empty());
    }
    bool offThreadSucceeded = true;
    std::thread worker([&] { offThreadSucceeded = runtime.CaptureGizmos(*adapter, false, commands); });
    worker.join();
    EXPECT_FALSE(offThreadSucceeded);
    EXPECT_TRUE(commands.empty());

    adapter->SetContext(nullptr, nullptr);
    EXPECT_FALSE(runtime.CaptureGizmos(*adapter, false, commands));
    EXPECT_TRUE(commands.empty());
    adapter->SetContext(&m_scene, &owner);
    EXPECT_TRUE(runtime.CaptureGizmos(*adapter, true, commands));
    EXPECT_TRUE(commands.empty());
    EXPECT_FALSE(adapter->IsRuntimeFaulted());
}

TEST_F(ManagedCompletionTest, EmptyGizmoCaptureRejectsNestedManagedEventScopeAndPreservesPublication)
{
    auto& publisher = m_scene.CreateGameObject("GizmoScopePublisher");
    auto& owner = m_scene.CreateGameObject("NestedEmptyGizmoOwner");
    auto* caller = Add(publisher, "cs-internal:ManagedFixtures.CompletionComponent");
    auto* adapter = Add(owner, "cs-internal:ManagedFixtures.HealthComponent");
    ASSERT_NE(caller, nullptr);
    ASSERT_NE(adapter, nullptr);
    Tick();
    ASSERT_EQ(Read(*caller, "connectCount").integer, 1);
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    std::vector<scene::ScriptDebugDrawCommand> commands;
    int deliveries = 0;
    bool regularSucceeded = true;
    bool selectedSucceeded = true;
    bool regularEmpty = false;
    bool selectedEmpty = false;
    const auto subscription = scene::ScriptEventBus::SubscribeRaw(adapter, "managed:System.Int32", [&](const void*) {
        ++deliveries;
        commands.emplace_back();
        regularSucceeded = runtime.CaptureGizmos(*adapter, false, commands);
        regularEmpty = commands.empty();
        commands.emplace_back();
        selectedSucceeded = runtime.CaptureGizmos(*adapter, true, commands);
        selectedEmpty = commands.empty();
    });
    ASSERT_TRUE(subscription.IsValid());
    EXPECT_TRUE(caller->ApplySettingsJson(R"({"command":3})"));
    Tick();
    scene::ScriptEventBus::Unsubscribe(subscription);

    EXPECT_EQ(deliveries, 1);
    EXPECT_FALSE(regularSucceeded);
    EXPECT_FALSE(selectedSucceeded);
    EXPECT_TRUE(regularEmpty);
    EXPECT_TRUE(selectedEmpty);
    EXPECT_EQ(Read(*caller, "eventSum").integer, 3);
    EXPECT_FALSE(caller->IsRuntimeFaulted());
    EXPECT_FALSE(adapter->IsRuntimeFaulted());
    EXPECT_TRUE(runtime.CaptureGizmos(*adapter, false, commands));
    EXPECT_TRUE(commands.empty());
    EXPECT_TRUE(runtime.CaptureGizmos(*adapter, true, commands));
    EXPECT_TRUE(commands.empty());
}

TEST_F(ManagedCompletionTest, EmptyGizmoCallbacksRejectDeletedOwnerAfterEntitySlotReuse)
{
    auto& owner = m_scene.CreateGameObject("ExpiredGizmoOwner");
    auto* adapter = Add(owner, "cs-internal:ManagedFixtures.HealthComponent");
    ASSERT_NE(adapter, nullptr);
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    const auto identity = owner.GetID();
    const auto guid = owner.instanceId;
    auto retained = std::move(owner.GetComponent<scene::ScriptComponent>()->scripts.front().script);
    ASSERT_EQ(retained.get(), adapter);
    ASSERT_TRUE(m_scene.DestroyGameObject(identity));
    auto& replacement = m_scene.CreateGameObject("ReplacementGizmoOwner");
    replacement.instanceId = guid;
    ASSERT_EQ(replacement.GetID().index, identity.index);
    EXPECT_NE(replacement.GetID().generation, identity.generation);
    std::vector<scene::ScriptDebugDrawCommand> commands(1);

    EXPECT_FALSE(runtime.CaptureGizmos(*adapter, false, commands));
    EXPECT_TRUE(commands.empty());
    EXPECT_FALSE(runtime.CaptureGizmos(*adapter, true, commands));
    EXPECT_TRUE(commands.empty());
    EXPECT_EQ(m_scene.GetGameObject(identity), nullptr);
}

TEST_F(ManagedCompletionTest, GizmoCallbackPresenceFollowsReloadAndRetainedGenerationActivation)
{
    auto& owner = m_scene.CreateGameObject("OriginalGizmoGeneration");
    auto* original = Add(owner, "cs-internal:ManagedFixtures.GizmoReloadMaskComponent");
    ASSERT_NE(original, nullptr);
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    std::vector<scene::ScriptDebugDrawCommand> commands;
    ASSERT_TRUE(runtime.CaptureGizmos(*original, false, commands));
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands.front().radius, 1.0f);
    ASSERT_TRUE(runtime.CaptureGizmos(*original, true, commands));
    EXPECT_TRUE(commands.empty());
    const auto originalGeneration = original->Generation();
    const auto originalInstance = original->InstanceId();
    const auto lease = runtime.RetainAppliedGeneration();
    ASSERT_EQ(lease, originalGeneration);

    scene::Scene play;
    auto& cloneOwner = play.CreateGameObject("ReloadedGizmoGeneration");
    auto& cloneEntry = cloneOwner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    cloneEntry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.GizmoReloadMaskComponent", cloneEntry.savedScriptId);
    ASSERT_NE(cloneEntry.script, nullptr);
    cloneEntry.script->SetContext(&play, &cloneOwner);
    auto* changed = runtime.FindAdapter(cloneEntry.script.get());
    ASSERT_NE(changed, nullptr);
    ASSERT_TRUE(changed->RefreshOwnerBinding());
    ASSERT_TRUE(runtime.Reload(play, FBZZ_MANAGED_FIXTURE_V2_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    EXPECT_NE(changed->Generation(), originalGeneration);
    ASSERT_TRUE(runtime.CaptureGizmos(*changed, false, commands));
    EXPECT_TRUE(commands.empty());
    ASSERT_TRUE(runtime.CaptureGizmos(*changed, true, commands));
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands.front().radius, 2.0f);
    EXPECT_FALSE(runtime.CaptureGizmos(*original, false, commands));
    EXPECT_TRUE(commands.empty());

    ASSERT_TRUE(runtime.ActivateRetainedGeneration(lease)) << runtime.LastDiagnostic();
    EXPECT_EQ(original->InstanceId(), originalInstance);
    ASSERT_TRUE(runtime.CaptureGizmos(*original, false, commands));
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands.front().radius, 1.0f);
    ASSERT_TRUE(runtime.CaptureGizmos(*original, true, commands));
    EXPECT_TRUE(commands.empty());
    EXPECT_FALSE(runtime.CaptureGizmos(*changed, true, commands));
    EXPECT_TRUE(commands.empty());
    runtime.ReleaseRetainedGeneration(lease);
    play.Clear();
}

TEST_F(ManagedCompletionTest, GizmosUseDetachedSettingsReadOnlyNativeScopeAndBoundedWorldCommands)
{
    auto& owner = m_scene.CreateGameObject("GizmoPreview");
    owner.transform.position = {3, 4, 5};
    scene::FlushWorldTransforms(owner);
    auto* adapter = Add(owner, "cs-internal:ManagedFixtures.GizmoProbeComponent");
    ASSERT_NE(adapter, nullptr);
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    std::vector<scene::ScriptDebugDrawCommand> commands;
    ASSERT_TRUE(runtime.CaptureGizmos(*adapter, false, commands)) << runtime.LastDiagnostic();
    ASSERT_EQ(commands.size(), 3u);
    EXPECT_EQ(commands[0].type, scene::ScriptDebugDrawType::Line);
    EXPECT_VEC3_NEAR(commands[0].a, (math::Vector3{3, 4, 5}), testkit::kTolerance);
    EXPECT_VEC3_NEAR(commands[0].b, (math::Vector3{3, 5, 5}), testkit::kTolerance);
    EXPECT_EQ(commands[1].type, scene::ScriptDebugDrawType::Sphere);
    EXPECT_EQ(commands[1].radius, 1);
    EXPECT_EQ(commands[2].type, scene::ScriptDebugDrawType::Box);
    EXPECT_TRUE(commands[0].depthTest);
    EXPECT_EQ(Read(*adapter, "radius").x, 1);
    ASSERT_TRUE(runtime.CaptureGizmos(*adapter, true, commands));
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_FALSE(commands[0].depthTest);
    for (const int command : {1, 2, 3}) {
        ASSERT_TRUE(adapter->ApplySettingsJson("{\"command\":" + std::to_string(command) + "}"));
        EXPECT_FALSE(runtime.CaptureGizmos(*adapter, false, commands));
        EXPECT_TRUE(commands.empty());
        EXPECT_FALSE(adapter->IsRuntimeFaulted());
        EXPECT_TRUE(adapter->enabled);
        EXPECT_VEC3_NEAR(owner.transform.position, (math::Vector3{3, 4, 5}), testkit::kTolerance);
    }
    ASSERT_TRUE(adapter->ApplySettingsJson(R"({"command":0})"));
    EXPECT_FALSE(runtime.CaptureGizmos(*adapter, false, commands, 2));
    EXPECT_TRUE(commands.empty());
    EXPECT_EQ(Read(*adapter, "radius").x, 1);
}

TEST_F(ManagedCompletionTest, WorldFamiliesUseActualClrFacadesFixedGeometryAndReadonlyDraw)
{
    scene::Script::SetInPlayMode(true);
    auto& owner = m_scene.CreateGameObject("WorldFamilyInterop");
    owner.transform.position = {0, 7, 0}; scene::FlushWorldTransforms(owner);
    auto& terrain = owner.AddComponent<scene::TerrainComponent>();
    terrain.columns = terrain.rows = 3; terrain.cellSize = 1; terrain.maxHeight = 10; terrain.InitFlat();
    auto& water = owner.AddComponent<scene::WaterComponent>(); water.enableGerstnerWaves = false;
    auto& vfx = owner.AddComponent<scene::VFXComponent>();
    auto& sequence = owner.AddComponent<scene::SequencePlayerComponent>();
    auto* adapter = Add(owner, "cs-internal:ManagedFixtures.WorldFamiliesComponent");
    ASSERT_NE(adapter, nullptr);
    Tick();
    ASSERT_FALSE(adapter->IsRuntimeFaulted()) << adapter->FaultDiagnostic();
    EXPECT_EQ(Read(*adapter, "connectionMask").integer, 15);
    EXPECT_NEAR(Read(*adapter, "waterHeight").x, 7, testkit::kTolerance);
    EXPECT_EQ(Read(*adapter, "sampled").integer, 1);
    adapter->OnFixedUpdate();
    ASSERT_FALSE(adapter->IsRuntimeFaulted()) << adapter->FaultDiagnostic();
    EXPECT_TRUE(terrain.heightDirty); EXPECT_TRUE(terrain.colliderDirty);
    EXPECT_NEAR(terrain.heightData[4], 0.4f, testkit::kTolerance);
    EXPECT_TRUE(vfx.playing); EXPECT_TRUE(vfx.restartRequested);
    ASSERT_EQ(vfx.pendingTriggers.size(), 1U); EXPECT_EQ(vfx.pendingTriggers[0], "Hit");
    EXPECT_TRUE(sequence.pendingPlay); EXPECT_EQ(sequence.pendingSeek, 1000000000000.125);
    sequence.time = sequence.pendingSeek;
    adapter->OnUpdate();
    ASSERT_FALSE(adapter->IsRuntimeFaulted()) << adapter->FaultDiagnostic();
    EXPECT_NEAR(Read(*adapter, "terrainHeight").x, 4, testkit::kTolerance);
    EXPECT_NEAR(Read(*adapter, "sampleY").x, 11, testkit::kTolerance);
    EXPECT_EQ(Read(*adapter, "preciseSequenceTime").integer, 1);
    std::vector<scene::ScriptDebugDrawCommand> commands;
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.CaptureGizmos(*adapter, false, commands)) << runtime.LastDiagnostic();
    ASSERT_EQ(commands.size(), 1U);
    EXPECT_VEC3_NEAR(commands[0].a, (math::Vector3{1, 11, 1}), testkit::kTolerance);
    ASSERT_TRUE(adapter->ApplySettingsJson(R"({"rejectDrawMutation":true})"));
    EXPECT_FALSE(runtime.CaptureGizmos(*adapter, false, commands));
    EXPECT_TRUE(commands.empty()); EXPECT_TRUE(vfx.playing);
    EXPECT_FALSE(adapter->IsRuntimeFaulted());
}

TEST_F(ManagedCompletionTest, RetainedEditGenerationActivatesWithoutReplacingParkedClrInstances)
{
    auto& originalOwner = m_scene.CreateGameObject("ParkedEditOwner");
    auto* original = Add(originalOwner, "cs-internal:ManagedFixtures.HealthComponent");
    ASSERT_NE(original, nullptr);
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    const auto originalInstance = original->InstanceId();
    const auto originalGeneration = original->Generation();
    const auto lease = runtime.RetainAppliedGeneration();
    ASSERT_EQ(lease, originalGeneration);
    scene::Scene play;
    auto& cloneOwner = play.CreateGameObject("PlayOwner");
    auto& cloneEntry = cloneOwner.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    cloneEntry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.HealthComponent", cloneEntry.savedScriptId);
    ASSERT_NE(cloneEntry.script, nullptr);
    cloneEntry.script->SetContext(&play, &cloneOwner);
    ASSERT_TRUE(runtime.Reload(play, FBZZ_MANAGED_FIXTURE_V2_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    const auto firstPlayGeneration = runtime.GetAppliedGeneration();
    EXPECT_NE(firstPlayGeneration, originalGeneration);
    ASSERT_TRUE(runtime.Reload(play, FBZZ_MANAGED_FIXTURE_V2_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    EXPECT_NE(runtime.GetAppliedGeneration(), firstPlayGeneration);
    EXPECT_EQ(original->Generation(), originalGeneration); EXPECT_EQ(original->InstanceId(), originalInstance);
    ASSERT_TRUE(runtime.ActivateRetainedGeneration(lease)) << runtime.LastDiagnostic();
    EXPECT_EQ(runtime.GetAppliedGeneration(), originalGeneration);
    EXPECT_EQ(original->Generation(), originalGeneration); EXPECT_EQ(original->InstanceId(), originalInstance);
    EXPECT_EQ(originalOwner.GetComponent<scene::ScriptComponent>()->scripts.front().script.get(), original);
    EXPECT_EQ(Read(*original, "health").integer, 10);
    runtime.ReleaseRetainedGeneration(lease);
    EXPECT_EQ(Read(*original, "health").integer, 10);
    EXPECT_FALSE(runtime.ActivateRetainedGeneration(lease));
    play.Clear();
}

TEST_F(ManagedCompletionTest, InitialPlayForkIsolatesManagedStaticsAndRestoresOriginalWithoutReloadCallbacks)
{
    auto& owner = m_scene.CreateGameObject("StaticDomainOwner");
    auto* original = Add(owner, "cs-internal:ManagedFixtures.StaticDomainProbeComponent");
    ASSERT_NE(original, nullptr);
    Tick();
    ASSERT_EQ(Read(*original, "observedUpdates").integer, 1);
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    const auto originalInstance = original->InstanceId();
    const auto originalGeneration = runtime.RetainAppliedGeneration();
    ASSERT_NE(originalGeneration, 0u);
    const std::string assembly = FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY;
    for (const uint64_t generation : {100ULL, 101ULL}) {
        scene::ManagedBridgeRequest load;
        load.op = scene::ManagedBridgeOp::LOAD_CANDIDATE; load.generation = generation; load.flags = 1;
        load.input = assembly.data(); load.inputBytes = static_cast<uint32_t>(assembly.size());
        scene::ManagedBridgeReply loaded;
        ASSERT_TRUE(runtime.Dispatch(load, loaded));
    }
    EXPECT_FALSE(runtime.ForkRetainedGenerationForPlay(originalGeneration));
    EXPECT_EQ(runtime.GetAppliedGeneration(), originalGeneration);
    EXPECT_EQ(original->InstanceId(), originalInstance);
    EXPECT_EQ(Read(*original, "observedUpdates").integer, 1);
    for (const uint64_t generation : {100ULL, 101ULL}) {
        scene::ManagedBridgeRequest retire; retire.op = scene::ManagedBridgeOp::RETIRE_GENERATION; retire.generation = generation;
        scene::ManagedBridgeReply retired;
        ASSERT_TRUE(runtime.Dispatch(retire, retired));
    }
    const std::string text = scene::SceneSerializer::SaveToText(m_scene);
    ASSERT_FALSE(text.empty());
    ASSERT_TRUE(runtime.ForkRetainedGenerationForPlay(originalGeneration)) << runtime.LastDiagnostic();
    EXPECT_NE(runtime.GetAppliedGeneration(), originalGeneration);
    auto play = scene::SceneSerializer::LoadDataFromText(text, "play-clone.fbzz");
    ASSERT_NE(play, nullptr);
    auto* playOwner = play->Find("StaticDomainOwner");
    ASSERT_NE(playOwner, nullptr);
    auto* clone = runtime.FindAdapter(playOwner->GetComponent<scene::ScriptComponent>()->scripts.front().script.get());
    ASSERT_NE(clone, nullptr);
    SystemContext playContext{*play, m_world, nullptr, nullptr, 0.25f, 0.02f, true, true};
    scene::ScriptSystem{}.Update(playContext);
    EXPECT_EQ(Read(*clone, "observedUpdates").integer, 1);
    scene::ScriptSystem{}.Update(playContext);
    EXPECT_EQ(Read(*clone, "observedUpdates").integer, 2);
    EXPECT_EQ(Read(*original, "observedUpdates").integer, 1);
    EXPECT_EQ(Read(*clone, "reloadCallbacks").integer, 0);
    play.reset();
    ASSERT_TRUE(runtime.ActivateRetainedGeneration(originalGeneration));
    runtime.ReleaseRetainedGeneration(originalGeneration);
    EXPECT_EQ(original->InstanceId(), originalInstance);
    Tick();
    EXPECT_EQ(Read(*original, "observedUpdates").integer, 2);
    EXPECT_EQ(Read(*original, "reloadCallbacks").integer, 0);
}

TEST_F(ManagedCompletionTest, TypedReferencesLoadForwardPeersAndReloadExactBirths)
{
    auto& owner = m_scene.CreateGameObject("TypedOwner");
    auto& bodyOwner = m_scene.CreateGameObject("TypedBody");
    bodyOwner.AddComponent<scene::RigidBodyComponent>();
    auto* first = Add(owner, "cs-internal:ManagedFixtures.TypedReferenceComponent");
    auto* second = Add(owner, "cs-internal:ManagedFixtures.TypedReferenceComponent");
    ASSERT_NE(first, nullptr); ASSERT_NE(second, nullptr);
    const std::string savedId = second->SavedScriptId(), originalBirth = second->ReferenceBirth();
    const std::string settings = "{\"target\":{\"$component\":{\"kind\":\"script\",\"ownerGuid\":\"" + owner.instanceId
        + "\",\"componentKind\":4294967295,\"savedScriptId\":\"" + savedId
        + "\",\"typeKey\":\"cs-internal:ManagedFixtures.TypedReferenceComponent\"}},\"bodies\":[{\"$component\":{\"kind\":\"native\",\"ownerGuid\":\""
        + bodyOwner.instanceId + "\",\"componentKind\":1,\"savedScriptId\":\"\",\"typeKey\":\"\"}}],\"material\":\"guid:missing-material\",\"texture\":\"guid:missing-texture\"}";
    ASSERT_TRUE(first->ApplySettingsJson(settings, scene::ManagedScriptAdapter::SettingsValidationPurpose::AUTHORING));
    std::string snapshot; ASSERT_TRUE(first->ReadSettingsSnapshot(snapshot));
    EXPECT_NE(snapshot.find("\"scriptBirth\":\"" + originalBirth + "\""), std::string::npos);
    EXPECT_NE(snapshot.find("\"componentLifetime\":\"" + std::to_string(m_scene.GetComponentLifetime<scene::RigidBodyComponent>(bodyOwner.GetID())) + "\""), std::string::npos);
    const std::string text = scene::SceneSerializer::SaveToText(m_scene); ASSERT_FALSE(text.empty());
    auto restored = scene::SceneSerializer::LoadDataFromText(text, "typed-references.fbzz"); ASSERT_NE(restored, nullptr);
    auto* restoredOwner = restored->Find("TypedOwner"); ASSERT_NE(restoredOwner, nullptr);
    auto* restoredEntries = restoredOwner->GetComponent<scene::ScriptComponent>(); ASSERT_NE(restoredEntries, nullptr); ASSERT_EQ(restoredEntries->scripts.size(), 2);
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    auto* restoredFirst = runtime.FindAdapter(restoredEntries->scripts[0].script.get());
    auto* restoredSecond = runtime.FindAdapter(restoredEntries->scripts[1].script.get()); ASSERT_NE(restoredFirst, nullptr); ASSERT_NE(restoredSecond, nullptr);
    ASSERT_TRUE(restoredFirst->ReadSettingsSnapshot(snapshot));
    EXPECT_EQ(restoredSecond->SavedScriptId(), savedId); EXPECT_NE(restoredSecond->ReferenceBirth(), originalBirth);
    EXPECT_NE(snapshot.find("\"scriptBirth\":\"" + restoredSecond->ReferenceBirth() + "\""), std::string::npos);
    EXPECT_NE(snapshot.find("guid:missing-material"), std::string::npos); EXPECT_NE(snapshot.find("guid:missing-texture"), std::string::npos);
    restored.reset();
    auto prefab = toml::parse(text);
    scene::RemapPrefabObjectReferences(prefab, {{owner.instanceId, "clone-owner-guid"}, {bodyOwner.instanceId, "clone-body-guid"}});
    std::ostringstream prefabText; prefabText << prefab;
    auto instance = scene::SceneSerializer::LoadDataFromText(prefabText.str(), "typed-prefab.fbzz"); ASSERT_NE(instance, nullptr);
    auto* instanceOwner = instance->Find("TypedOwner"); ASSERT_NE(instanceOwner, nullptr);
    auto* instanceEntries = instanceOwner->GetComponent<scene::ScriptComponent>(); ASSERT_NE(instanceEntries, nullptr); ASSERT_EQ(instanceEntries->scripts.size(), 2);
    auto* instanceFirst = runtime.FindAdapter(instanceEntries->scripts[0].script.get()); ASSERT_NE(instanceFirst, nullptr);
    ASSERT_TRUE(instanceFirst->ReadSettingsSnapshot(snapshot));
    EXPECT_NE(snapshot.find("clone-owner-guid"), std::string::npos); EXPECT_NE(snapshot.find("clone-body-guid"), std::string::npos);
    EXPECT_EQ(snapshot.find(owner.instanceId), std::string::npos); EXPECT_EQ(snapshot.find(bodyOwner.instanceId), std::string::npos);
    instance.reset();
    const uint64_t previousPeer = second->InstanceId();
    ASSERT_TRUE(runtime.Reload(m_scene, FBZZ_MANAGED_FIXTURE_V2_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    EXPECT_NE(second->InstanceId(), previousPeer); EXPECT_EQ(second->ReferenceBirth(), originalBirth);
    ASSERT_TRUE(first->ReadSettingsSnapshot(snapshot));
    EXPECT_NE(snapshot.find("\"scriptBirth\":\"" + originalBirth + "\""), std::string::npos);
    EXPECT_NE(snapshot.find("\"scriptInstanceId\":\"" + std::to_string(second->InstanceId()) + "\""), std::string::npos);
}

TEST_F(ManagedCompletionTest, RuntimeDataAssetLoadStreamsLargeJapanesePathWithoutRunningConstructor)
{
    const auto path = m_temp.Path() / std::filesystem::path(u8"大きな設定.fzdata");
    toml::table data; data.insert("type", "ManagedFixtures.LargePortableProfileDataAsset");
    data.insert("text", std::string(18U * 1024U * 1024U, 'x') + "終端");
    toml::array values; for (int index = 0; index <= 40000; ++index) values.push_back(index); data.insert("values", std::move(values));
    std::ostringstream file; file << data; ASSERT_GT(file.str().size(), 16U * 1024U * 1024U);
    ASSERT_TRUE(util::FileSystem::WriteText(path, file.str()));
    auto& owner = m_scene.CreateGameObject("LargeAssetReader");
    auto* adapter = Add(owner, "cs-internal:ManagedFixtures.LargeDataAssetLoadComponent"); ASSERT_NE(adapter, nullptr);
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"profile\":\"" + util::FileSystem::PathToUtf8(path) + "\"}"));
    Tick();
    EXPECT_FALSE(adapter->IsRuntimeFaulted()) << adapter->FaultDiagnostic();
    EXPECT_EQ(Read(*adapter, "textLength").integer, 18 * 1024 * 1024 + 2);
    EXPECT_EQ(Read(*adapter, "lastValue").integer, 40000); EXPECT_EQ(Read(*adapter, "tailValid").integer, 1);
    EXPECT_EQ(Read(*adapter, "constructorCalls").integer, 0);
}

} /// @note namespace fbzz::tests
