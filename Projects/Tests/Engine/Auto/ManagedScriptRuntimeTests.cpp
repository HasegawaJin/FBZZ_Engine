/// @file    ManagedScriptRuntimeTests.cpp
/// @brief   実 CoreCLR と既存 Script/Serializer の縦断接続を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <Engine/Scene/ScriptComponent.hpp>
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Core/Profiler/Profiler.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Profiler/ScriptProfiler.hpp>
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Engine/Scene/TomlReflector.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Physics/World.hpp>
#include <algorithm>
#include <bit>
#include <limits>
#include <thread>

namespace fbzz::tests {
namespace {


} /// @note namespace

class ManagedScriptRuntimeTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    physics::World m_world;
    testkit::TempDir m_temp{"managed-script"};
    float m_previousDelta = 0;
    bool m_previousPlay = false;
    bool m_previousPerformance = false;
    bool m_previousScriptRecording = false;

    void SetUp() override
    {
        EngineFixture::SetUp();
        m_previousDelta = Time::deltaTime;
        m_previousPlay = scene::Script::IsInPlayMode();
        m_previousPerformance = profiler::Profiler::IsEnabled();
        m_previousScriptRecording = scene::ScriptProfiler::IsRecording();
        Time::deltaTime = 0.25f;
        scene::Script::SetInPlayMode(false);
    }

    void TearDown() override
    {
        scene::ManagedScriptRuntime::Instance().Unload(&m_scene);
        m_scene.Clear();
        Time::deltaTime = m_previousDelta;
        scene::Script::SetInPlayMode(m_previousPlay);
        scene::ScriptProfiler::RequestRecording(m_previousScriptRecording);
        scene::ScriptProfiler::RequestClear();
        scene::ScriptProfiler::BeginFrame(2, 0.0);
        scene::ScriptProfiler::EndFrame();
        profiler::Profiler::SetEnabled(m_previousPerformance);
        EngineFixture::TearDown();
    }

    scene::GameObject& AddMissing(const char* name, const char* type, const char* fields)
    {
        auto& object = m_scene.CreateGameObject(name);
        auto& component = object.AddComponent<scene::ScriptComponent>();
        auto& entry = component.scripts.emplace_back();
        entry.serialized = std::make_unique<scene::SerializedScriptData>(scene::SerializedScriptData{type, fields, true});
        return object;
    }

    static toml::table Fields(scene::Script& script)
    {
        toml::table fields;
        util::TomlWriteReflector reflector(fields);
        script.Reflect(reflector);
        return fields;
    }

    void Tick()
    {
        SystemContext context{m_scene, m_world, nullptr, nullptr, 0.25f, 0.02f, true, true};
        scene::ScriptSystem{}.Update(context);
    }

    scene::ManagedScriptAdapter* Attach(scene::GameObject& object, const char* type)
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
};

TEST_F(ManagedScriptRuntimeTest, PreservesInheritedUpdatesAndCleanupWithKnownAbsentFixedAndLateCallbacks)
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
    ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    auto& owner = m_scene.CreateGameObject("LifecyclePresence");
    auto* adapter = Attach(owner, "cs-internal:ManagedFixtures.LifecyclePresenceComponent");
    ASSERT_NE(adapter, nullptr);
    adapter->OnUpdate(); adapter->OnFixedUpdate(); adapter->OnLateUpdate();
    EXPECT_FALSE(adapter->IsRuntimeFaulted());
    EXPECT_EQ(Fields(*adapter)["updateCount"].value_or(int64_t{}), 1);
    EXPECT_EQ(Fields(*adapter)["fixedCount"].value_or(int64_t{}), 0);
    EXPECT_EQ(Fields(*adapter)["lateCount"].value_or(int64_t{}), 0);
    adapter->OnDisable(); adapter->OnDestroy();
    EXPECT_EQ(Fields(*adapter)["disableCount"].value_or(int64_t{}), 1);
    EXPECT_EQ(Fields(*adapter)["destroyCount"].value_or(int64_t{}), 1);

    for (const char* type : {"cs-internal:ManagedFixtures.InheritedOnlyCallbackComponent", "cs-internal:ManagedFixtures.InheritedCallbackComponent"}) {
        auto& inheritedOwner = m_scene.CreateGameObject(type);
        auto* inherited = Attach(inheritedOwner, type);
        ASSERT_NE(inherited, nullptr);
        inherited->OnUpdate(); inherited->OnFixedUpdate(); inherited->OnLateUpdate();
        EXPECT_FALSE(inherited->IsRuntimeFaulted());
        scene::ManagedBridgeRequest request;
        request.op = scene::ManagedBridgeOp::READ_FIELD;
        request.generation = inherited->Generation(); request.instanceId = inherited->InstanceId();
        const std::string field = std::string(type).find("InheritedOnly") != std::string::npos ? "ManagedFixtures.BaseCallbackComponent.baseCalls" : "ManagedFixtures.InheritedCallbackComponent.derivedCalls";
        request.input = field.data(); request.inputBytes = static_cast<uint32_t>(field.size());
        scene::ManagedBridgeReply reply;
        ASSERT_TRUE(runtime.Dispatch(request, reply)) << runtime.LastDiagnostic();
        EXPECT_EQ(reply.integer, 1);
    }
}

TEST_F(ManagedScriptRuntimeTest, ReloadsUpdatePresenceAndReactivatesItsRetainedGenerationWithoutReplacingAdapters)
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
    ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    auto& owner = m_scene.CreateGameObject("ReloadedLifecycle");
    auto* adapter = Attach(owner, "cs-internal:ManagedFixtures.LifecyclePresenceComponent");
    ASSERT_NE(adapter, nullptr);
    scene::Scene preview;
    auto& previewOwner = preview.CreateGameObject("RetainedLifecycle");
    auto held = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.LifecyclePresenceComponent", "11111111-1111-4111-8111-111111111111");
    ASSERT_NE(held, nullptr);
    held->SetContext(&preview, &previewOwner);
    auto* previous = runtime.FindAdapter(held.get());
    ASSERT_NE(previous, nullptr);
    ASSERT_TRUE(previous->RefreshOwnerBinding());
    const uint64_t generation = runtime.RetainAppliedGeneration();
    ASSERT_NE(generation, 0U);
    struct GenerationLease {
        scene::ManagedScriptRuntime& runtime;
        uint64_t generation;
        ~GenerationLease() { runtime.ReleaseRetainedGeneration(generation); }
    } lease{runtime, generation};
    ASSERT_TRUE(runtime.Invoke(*adapter, scene::ManagedCallback::UPDATE));
    ASSERT_TRUE(runtime.Invoke(*adapter, scene::ManagedCallback::FIXED_UPDATE));
    ASSERT_TRUE(runtime.Invoke(*adapter, scene::ManagedCallback::LATE_UPDATE));
    const auto* original = adapter;
    ASSERT_TRUE(runtime.Reload(m_scene, FBZZ_MANAGED_FIXTURE_V2_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    EXPECT_EQ(runtime.FindAdapter(owner.GetComponent<scene::ScriptComponent>()->scripts.front().script.get()), original);
    EXPECT_NE(adapter->Generation(), generation);
    EXPECT_FALSE(runtime.Invoke(*previous, scene::ManagedCallback::FIXED_UPDATE));
    EXPECT_NE(runtime.LastDiagnostic().find("unpublished"), std::string::npos);
    ASSERT_TRUE(runtime.Invoke(*adapter, scene::ManagedCallback::UPDATE));
    ASSERT_TRUE(runtime.Invoke(*adapter, scene::ManagedCallback::FIXED_UPDATE));
    ASSERT_TRUE(runtime.Invoke(*adapter, scene::ManagedCallback::LATE_UPDATE));
    EXPECT_EQ(Fields(*adapter)["updateCount"].value_or(int64_t{}), 1);
    EXPECT_EQ(Fields(*adapter)["fixedCount"].value_or(int64_t{}), 1);
    EXPECT_EQ(Fields(*adapter)["lateCount"].value_or(int64_t{}), 1);

    ASSERT_TRUE(runtime.ActivateRetainedGeneration(generation)) << runtime.LastDiagnostic();
    ASSERT_TRUE(runtime.Invoke(*previous, scene::ManagedCallback::UPDATE));
    ASSERT_TRUE(runtime.Invoke(*previous, scene::ManagedCallback::FIXED_UPDATE));
    ASSERT_TRUE(runtime.Invoke(*previous, scene::ManagedCallback::LATE_UPDATE));
    EXPECT_EQ(Fields(*previous)["updateCount"].value_or(int64_t{}), 1);
    EXPECT_EQ(Fields(*previous)["fixedCount"].value_or(int64_t{}), 0);
    EXPECT_EQ(Fields(*previous)["lateCount"].value_or(int64_t{}), 0);
    EXPECT_FALSE(runtime.Invoke(*adapter, scene::ManagedCallback::UPDATE));
}

TEST_F(ManagedScriptRuntimeTest, DoesNotHideHostOnlyFaultsBehindAnAbsentUpdateAfterOwnerRebinding)
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
    ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    auto& owner = m_scene.CreateGameObject("HostOnlyFault");
    auto& replacementOwner = m_scene.CreateGameObject("NewOwner");
    auto& healthyOwner = m_scene.CreateGameObject("HealthySibling");
    auto* adapter = Attach(owner, "cs-internal:ManagedFixtures.LifecyclePresenceComponent");
    auto* healthy = Attach(healthyOwner, "cs-internal:ManagedFixtures.HealthComponent");
    ASSERT_NE(adapter, nullptr); ASSERT_NE(healthy, nullptr);
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"failAwake\":true}"));
    EXPECT_FALSE(runtime.Invoke(*adapter, scene::ManagedCallback::AWAKE));
    EXPECT_FALSE(adapter->IsRuntimeFaulted());
    EXPECT_FALSE(runtime.Invoke(*adapter, scene::ManagedCallback::FIXED_UPDATE));
    EXPECT_NE(runtime.LastDiagnostic().find("faulted"), std::string::npos);
    adapter->SetContext(&m_scene, &replacementOwner);
    ASSERT_TRUE(adapter->RefreshOwnerBinding());
    EXPECT_FALSE(runtime.Invoke(*adapter, scene::ManagedCallback::LATE_UPDATE));
    EXPECT_NE(runtime.LastDiagnostic().find("faulted"), std::string::npos);
    ASSERT_TRUE(runtime.Invoke(*healthy, scene::ManagedCallback::UPDATE));
    ASSERT_TRUE(runtime.Invoke(*healthy, scene::ManagedCallback::FIXED_UPDATE));
    ASSERT_TRUE(runtime.Invoke(*healthy, scene::ManagedCallback::LATE_UPDATE));
    adapter->OnFixedUpdate();
    EXPECT_TRUE(adapter->IsRuntimeFaulted());
    EXPECT_FALSE(adapter->enabled);
    ASSERT_TRUE(runtime.Invoke(*adapter, scene::ManagedCallback::DESTROY));
    EXPECT_EQ(Fields(*adapter)["destroyCount"].value_or(int64_t{}), 1);
}

TEST_F(ManagedScriptRuntimeTest, MissingUpdatesStillRejectDetachedOwnersWorkerThreadsAndReleasedInstances)
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
    ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    auto& owner = m_scene.CreateGameObject("AbsentUpdates");
    auto* adapter = Attach(owner, "cs-internal:ManagedFixtures.HealthComponent");
    ASSERT_NE(adapter, nullptr);
    ASSERT_TRUE(runtime.Invoke(*adapter, scene::ManagedCallback::UPDATE));
    bool workerResult = true;
    std::thread worker([&] { workerResult = runtime.Invoke(*adapter, scene::ManagedCallback::UPDATE); });
    worker.join();
    EXPECT_FALSE(workerResult);
    adapter->SetContext(nullptr, nullptr);
    EXPECT_FALSE(runtime.Invoke(*adapter, scene::ManagedCallback::FIXED_UPDATE));
    adapter->SetContext(&m_scene, &owner);
    ASSERT_TRUE(adapter->RefreshOwnerBinding());
    ASSERT_TRUE(runtime.Invoke(*adapter, scene::ManagedCallback::LATE_UPDATE));
    scene::ManagedBridgeRequest request;
    request.op = scene::ManagedBridgeOp::RELEASE_INSTANCE;
    request.generation = adapter->Generation(); request.instanceId = adapter->InstanceId(); request.ownerId = adapter->OwnerId();
    scene::ManagedBridgeReply reply;
    ASSERT_TRUE(runtime.Dispatch(request, reply));
    EXPECT_FALSE(runtime.Invoke(*adapter, scene::ManagedCallback::UPDATE));
}

TEST_F(ManagedScriptRuntimeTest, RejectsInvalidSaveFieldsWithoutChangingEarlierSnapshotsOrTheExistingFile)
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
    ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    auto& firstOwner = m_scene.CreateGameObject("EarlierValid");
    auto& secondOwner = m_scene.CreateGameObject("LaterInvalid");
    auto* first = Attach(firstOwner, "cs-internal:ManagedFixtures.SaveFailureComponent");
    auto* second = Attach(secondOwner, "cs-internal:ManagedFixtures.SaveFailureComponent");
    ASSERT_NE(first, nullptr); ASSERT_NE(second, nullptr);
    auto& firstEntry = firstOwner.GetComponent<scene::ScriptComponent>()->scripts.front();
    auto& secondEntry = secondOwner.GetComponent<scene::ScriptComponent>()->scripts.front();
    firstEntry.serialized = std::make_unique<scene::SerializedScriptData>(scene::SerializedScriptData{
        first->GetTypeName(), "futureSetting = [10, 20]\n", true});
    const auto path = m_temp.File("preserved.scene").generic_string();
    ASSERT_TRUE(scene::SceneSerializer::Save(m_scene, path));
    ASSERT_NE(firstEntry.serialized, nullptr); ASSERT_NE(secondEntry.serialized, nullptr);
    const std::string firstSaved = firstEntry.serialized->fieldsToml;
    const std::string secondSaved = secondEntry.serialized->fieldsToml;
    ASSERT_NE(firstSaved.find("futureSetting"), std::string::npos);
    std::string originalFile;
    ASSERT_TRUE(util::FileSystem::ReadText(path, originalFile));
    ASSERT_TRUE(first->ApplySettingsJson("{\"speed\":4}"));

    /// @note Mode 7 is malformed UTF-16, not a valid large string; total field length is no longer a save rejection criterion.
    for (const int mode : {1, 2, 3, 5, 6, 7}) {
        SCOPED_TRACE(mode);
        ASSERT_TRUE(second->ApplySettingsJson("{\"failureMode\":" + std::to_string(mode)
            + ",\"settings\":{\"value\":1,\"label\":\"valid\",\"next\":null},\"speed\":2,\"values\":[],\"note\":\"valid\"}"));
        std::string diagnostic;
        const std::string failed = scene::SceneSerializer::SaveToText(m_scene, {}, diagnostic);

        EXPECT_TRUE(failed.empty());
        EXPECT_NE(diagnostic.find("LaterInvalid"), std::string::npos);
        EXPECT_NE(diagnostic.find("SaveFailureComponent"), std::string::npos);
        EXPECT_EQ(firstEntry.serialized->fieldsToml, firstSaved);
        EXPECT_EQ(secondEntry.serialized->fieldsToml, secondSaved);
        EXPECT_FALSE(scene::SceneSerializer::Save(m_scene, path));
        std::string afterFailure;
        ASSERT_TRUE(util::FileSystem::ReadText(path, afterFailure));
        EXPECT_EQ(afterFailure, originalFile);
    }
    ASSERT_TRUE(second->ApplySettingsJson("{\"failureMode\":0,\"settings\":{\"value\":1,\"label\":\"valid\",\"next\":null},\"speed\":2,\"values\":[],\"note\":\"valid\"}"));

    std::string diagnostic = "previous failure";
    const std::string recovered = scene::SceneSerializer::SaveToText(m_scene, {}, diagnostic);

    EXPECT_FALSE(recovered.empty());
    EXPECT_TRUE(diagnostic.empty());
    EXPECT_NE(firstEntry.serialized->fieldsToml, firstSaved);
    EXPECT_NE(firstEntry.serialized->fieldsToml.find("futureSetting"), std::string::npos);
}

TEST_F(ManagedScriptRuntimeTest, RejectsNewBeforeSerializeFaultButCanSaveValidSettingsFromTheStoppedScript)
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
    ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    auto& owner = m_scene.CreateGameObject("SerializationFault");
    auto* adapter = Attach(owner, "cs-internal:ManagedFixtures.SaveFailureComponent");
    ASSERT_NE(adapter, nullptr);
    ASSERT_FALSE(scene::SceneSerializer::SaveToText(m_scene).empty());
    auto& entry = owner.GetComponent<scene::ScriptComponent>()->scripts.front();
    const std::string preserved = entry.serialized->fieldsToml;
    ASSERT_TRUE(adapter->ApplySettingsJson("{\"failureMode\":4}"));

    std::string diagnostic;
    const std::string failed = scene::SceneSerializer::SaveToText(m_scene, {}, diagnostic);

    EXPECT_TRUE(failed.empty());
    EXPECT_NE(diagnostic.find("OnBeforeSerialize"), std::string::npos);
    EXPECT_EQ(entry.serialized->fieldsToml, preserved);
    EXPECT_FALSE(adapter->enabled);
    EXPECT_FALSE(adapter->FaultDiagnostic().empty());
    EXPECT_FALSE(scene::SceneSerializer::SaveToText(m_scene).empty());
}

/// @note 一つの縦断経路で世代を二つだけ使用する。GC 待ち上限を試験都合で解除せず、失敗候補も含めて通常の解放契約を通す。
TEST_F(ManagedScriptRuntimeTest, RunsSavesReloadsAndRejectsInvalidCodeWithoutReplacingAdapters)
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
    const auto expectCatalog = [&](uint64_t generation, bool alive) {
        scene::ManagedBridgeRequest request;
        request.op = scene::ManagedBridgeOp::GET_TYPE;
        request.generation = generation;
        scene::ManagedBridgeReply reply;
        std::string type;
        if (alive) {
            EXPECT_TRUE(runtime.Dispatch(request, reply, &type)) << runtime.LastDiagnostic();
        } else {
            EXPECT_FALSE(runtime.Dispatch(request, reply, &type));
            EXPECT_EQ(reply.status, scene::ManagedBridgeStatus::STALE_GENERATION);
        }
    };
    auto& mover = AddMissing("Mover", "cs-internal:ManagedFixtures.LifecycleComponent", "speed = 6.0\nprivateSpeed = 3.0\n");
    auto& settingsObject = AddMissing("Settings", "cs-internal:ManagedFixtures.SettingsComponent",
        "speed = 9.0\ncount = 12\nactive = false\nlabel = 'saved settings'\noffset = [10.0, 20.0, 30.0]\nstableSpeed = 5.0\n");
    ASSERT_TRUE(runtime.Reload(m_scene, FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    ASSERT_TRUE(runtime.IsLoaded());
    EXPECT_FALSE(runtime.Shutdown());
    EXPECT_TRUE(runtime.IsLoaded());
    auto& moverEntry = mover.GetComponent<scene::ScriptComponent>()->scripts.front();
    auto& settingsEntry = settingsObject.GetComponent<scene::ScriptComponent>()->scripts.front();
    ASSERT_NE(moverEntry.script, nullptr);
    ASSERT_NE(settingsEntry.script, nullptr);
    scene::Script* originalAdapter = moverEntry.script.get();
    const std::string originalInspectionId = moverEntry.inspectionId;
    auto fields = Fields(*moverEntry.script);
    EXPECT_DOUBLE_EQ(fields["speed"].value_or(0.0), 6.0);
    EXPECT_DOUBLE_EQ(fields["privateSpeed"].value_or(0.0), 3.0);
    EXPECT_FALSE(fields.contains("runtimeElapsed"));
    auto settings = Fields(*settingsEntry.script);
    EXPECT_DOUBLE_EQ(settings["stableSpeed"].value_or(0.0), 5.0);
    EXPECT_EQ(settings["count"].value_or(int64_t{}), 12);
    EXPECT_FALSE(settings["active"].value_or(true));
    EXPECT_FALSE(settings.contains("transient"));
    EXPECT_FALSE(settings.contains("runtimeElapsed"));
    EXPECT_FALSE(settings.contains("readOnly"));
    ASSERT_NE(settings["offset"].as_array(), nullptr);
    EXPECT_DOUBLE_EQ((*settings["offset"].as_array())[2].value_or(0.0), 30.0);
    Tick();
    EXPECT_VEC3_NEAR(mover.transform.position, (math::Vector3{1.5f, 0, 0}), testkit::kTolerance);
    fields = Fields(*moverEntry.script);
    EXPECT_EQ(fields["awakeCount"].value_or(int64_t{}), 1);
    EXPECT_EQ(fields["startCount"].value_or(int64_t{}), 1);
    EXPECT_EQ(fields["updateCount"].value_or(int64_t{}), 1);

    const auto savedPath = m_temp.File("managed.fbzz").generic_string();
    ASSERT_TRUE(scene::SceneSerializer::Save(m_scene, savedPath));
    auto restored = scene::SceneSerializer::LoadData(savedPath);
    ASSERT_NE(restored, nullptr);
    auto* restoredSettings = restored->Find("Settings");
    ASSERT_NE(restoredSettings, nullptr);
    auto* restoredComponent = restoredSettings->GetComponent<scene::ScriptComponent>();
    ASSERT_NE(restoredComponent, nullptr);
    ASSERT_NE(restoredComponent->scripts.front().script, nullptr);
    settings = Fields(*restoredComponent->scripts.front().script);
    EXPECT_DOUBLE_EQ(settings["stableSpeed"].value_or(0.0), 5.0);
    EXPECT_EQ(settings["label"].value_or(std::string{}), "saved settings");
    ASSERT_NE(settings["offset"].as_array(), nullptr);
    EXPECT_DOUBLE_EQ((*settings["offset"].as_array())[0].value_or(0.0), 10.0);
    EXPECT_DOUBLE_EQ((*settings["offset"].as_array())[1].value_or(0.0), 20.0);
    restored.reset();

    /// @note factory が返す既知の native Adapter 型を使用し、managed 型への native pointer cast は行わない。
    auto& adapter = static_cast<scene::ManagedScriptAdapter&>(*moverEntry.script);
    /// @note forged INVOKE は CLR/user callback へ入る前に拒否する。Host のみの fault や callback の部分的変更を残さない。
    scene::ManagedBridgeRequest invalidInvoke;
    invalidInvoke.op = scene::ManagedBridgeOp::INVOKE;
    invalidInvoke.generation = adapter.Generation();
    invalidInvoke.instanceId = adapter.InstanceId();
    invalidInvoke.ownerId = adapter.OwnerId();
    invalidInvoke.callId = 123;
    invalidInvoke.index = static_cast<uint32_t>(scene::ManagedCallback::UPDATE);
    scene::ManagedBridgeReply reply;
    const math::Vector3 beforeInvalidScope = mover.transform.position;
    const auto updatesBeforeInvalidScope = Fields(adapter)["updateCount"].value_or(int64_t{});
    EXPECT_FALSE(runtime.Dispatch(invalidInvoke, reply));
    EXPECT_EQ(reply.status, scene::ManagedBridgeStatus::INVALID_SCOPE);
    EXPECT_VEC3_NEAR(mover.transform.position, beforeInvalidScope, testkit::kTolerance);
    EXPECT_EQ(Fields(adapter)["updateCount"].value_or(int64_t{}), updatesBeforeInvalidScope);
    EXPECT_FALSE(adapter.IsRuntimeFaulted());
    ASSERT_TRUE(runtime.Invoke(adapter, scene::ManagedCallback::AFTER_DESERIALIZE)) << runtime.LastDiagnostic();
    EXPECT_FALSE(adapter.IsRuntimeFaulted());

    scene::ManagedBridgeRequest invalidField;
    invalidField.op = scene::ManagedBridgeOp::WRITE_FIELD;
    invalidField.generation = adapter.Generation();
    invalidField.instanceId = adapter.InstanceId();
    invalidField.index = static_cast<uint32_t>(scene::ManagedValueKind::FLOAT);
    invalidField.input = "speed";
    invalidField.inputBytes = 5;
    invalidField.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(runtime.Dispatch(invalidField, reply));
    EXPECT_DOUBLE_EQ(Fields(adapter)["speed"].value_or(0.0), 6.0);

    auto& throwingObject = m_scene.CreateGameObject("Throwing");
    auto& throwingEntry = throwingObject.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    throwingEntry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.ThrowingCallbackComponent", throwingEntry.savedScriptId);
    ASSERT_NE(throwingEntry.script, nullptr);
    throwingEntry.script->SetContext(&m_scene, &throwingObject);
    profiler::Profiler::SetEnabled(true);
    scene::ScriptProfiler::RequestRecording(true);
    scene::ScriptProfiler::RequestClear();
    profiler::Profiler::BeginFrame(1, 0.0);
    scene::ScriptProfiler::BeginFrame(1, 0.0);
    EXPECT_FALSE(throwingEntry.script->ExecuteProfiledCallback(&scene::Script::OnUpdate,
        scene::ScriptCallbackKind::UPDATE, "OnUpdate"));
    EXPECT_TRUE(throwingEntry.script->IsRuntimeFaulted());
    EXPECT_FALSE(throwingEntry.script->enabled);
    EXPECT_EQ(Fields(*throwingEntry.script)["deserializeCount"].value_or(int64_t{}), 0);
    EXPECT_FALSE(throwingEntry.script->ExecuteProfiledCallback(&scene::Script::OnUpdate,
        scene::ScriptCallbackKind::UPDATE, "OnUpdate"));
    EXPECT_EQ(Fields(*throwingEntry.script)["updateCount"].value_or(int64_t{}), 1);
    scene::ScriptProfiler::EndFrame();
    profiler::Profiler::EndFrame();
    const auto& snapshot = scene::ScriptProfiler::GetSnapshot();
    ASSERT_EQ(snapshot.samples.size(), 1u);
    EXPECT_EQ(snapshot.samples.front().status, profiler::ProfileSampleStatus::FAULTED);
    scene::ScriptProfiler::RequestRecording(false);

    /// @note 別 Scene と未適用 preview の両方が旧世代を保持しても、最後の個体の解放で catalog を退役させる。
    const uint64_t oldGeneration = adapter.Generation();
    scene::Scene sharedScene;
    auto& sharedObject = sharedScene.CreateGameObject("Shared");
    auto& sharedEntry = sharedObject.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    sharedEntry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.SettingsComponent", sharedEntry.savedScriptId);
    ASSERT_NE(sharedEntry.script, nullptr);
    sharedEntry.script->SetContext(&sharedScene, &sharedObject);
    scene::ScriptEntry preparedEntry;
    std::unique_ptr<scene::Script> prepared = scene::ScriptFactory::Create(
        "cs-internal:ManagedFixtures.SettingsComponent", preparedEntry.savedScriptId);
    ASSERT_NE(prepared, nullptr);
    prepared->SetContext(&m_scene, &settingsObject);
    scene::Script::SetInPlayMode(false);
    ASSERT_TRUE(runtime.Reload(m_scene, FBZZ_MANAGED_FIXTURE_V2_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    expectCatalog(oldGeneration, true);
    prepared.reset();
    expectCatalog(oldGeneration, true);
    sharedScene.Clear();
    expectCatalog(oldGeneration, false);
    expectCatalog(runtime.GetAppliedGeneration(), true);
    EXPECT_EQ(moverEntry.script.get(), originalAdapter);
    EXPECT_EQ(moverEntry.inspectionId, originalInspectionId);
    EXPECT_DOUBLE_EQ(Fields(*moverEntry.script)["speed"].value_or(0.0), 6.0);
    EXPECT_TRUE(throwingEntry.script->IsRuntimeFaulted());
    EXPECT_FALSE(throwingEntry.script->enabled);
    EXPECT_EQ(Fields(*throwingEntry.script)["deserializeCount"].value_or(int64_t{}), 0);
    Tick();
    EXPECT_VEC3_NEAR(mover.transform.position, (math::Vector3{3.0f, 0, 0}), testkit::kTolerance);
    scene::Script::SetInPlayMode(false);
    const uint64_t validGeneration = adapter.Generation();
    EXPECT_FALSE(runtime.Reload(m_scene, FBZZ_MANAGED_INVALID_TEST_ASSEMBLY, true));
    EXPECT_EQ(moverEntry.script.get(), originalAdapter);
    EXPECT_EQ(adapter.Generation(), validGeneration);
    Tick();
    EXPECT_VEC3_NEAR(mover.transform.position, (math::Vector3{4.5f, 0, 0}), testkit::kTolerance);

    /// @note catalog 解除後も preview 個体が残る場合、現行だった世代も最後の解放まで退役を遅らせる。
    scene::ScriptEntry pendingPreviewEntry;
    std::unique_ptr<scene::Script> pendingPreview = scene::ScriptFactory::Create(
        "cs-internal:ManagedFixtures.SettingsComponent", pendingPreviewEntry.savedScriptId);
    ASSERT_NE(pendingPreview, nullptr);
    pendingPreview->SetContext(&m_scene, &settingsObject);
    runtime.Unload(&m_scene);
    EXPECT_FALSE(runtime.IsLoaded());
    EXPECT_EQ(moverEntry.script, nullptr);
    ASSERT_NE(moverEntry.serialized, nullptr);
    EXPECT_EQ(moverEntry.serialized->type, "cs-internal:ManagedFixtures.LifecycleComponent");
    EXPECT_NE(moverEntry.serialized->fieldsToml.find("speed"), std::string::npos);
    EXPECT_FALSE(runtime.IsCollectionPending());
    expectCatalog(validGeneration, true);
    EXPECT_FALSE(runtime.Shutdown());
    pendingPreview.reset();
    expectCatalog(validGeneration, false);
    EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic();
}

TEST_F(ManagedScriptRuntimeTest, PreservesReferenceSlotsAndArraysAndRejectsStaleObjectsAndPartialSettings)
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY)) << runtime.LastDiagnostic();
    ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    std::string key;
    ASSERT_TRUE(scene::ScriptFactory::ResolveTypeName("HealthComponent", key));
    EXPECT_EQ(key, "cs-internal:ManagedFixtures.HealthComponent");
    EXPECT_EQ(scene::ScriptFactory::DisplayName(key), "ManagedFixtures.HealthComponent");
    auto& target = m_scene.CreateGameObject("Target");
    const std::string targetGuid = target.instanceId;
    const scene::EntityID targetEntity = target.GetID();
    /// @note inactive の対象は ScriptSystem の Awake より先でも取得でき、所有者接続の遅延を lookup 側で補完する。
    target.SetActive(false);
    auto& healthEntry = target.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    healthEntry.script = scene::ScriptFactory::Create("cs-internal:ManagedFixtures.HealthComponent", healthEntry.savedScriptId);
    auto* health = runtime.FindAdapter(healthEntry.script.get());
    ASSERT_NE(health, nullptr);
    EXPECT_FALSE(health->HasLiveContext());
    auto& owner = m_scene.CreateGameObject("References");
    auto* references = Attach(owner, "cs-internal:ManagedFixtures.ReferenceComponent");
    ASSERT_NE(references, nullptr);
    const std::string missingGuid = "00000000-0000-4000-8000-000000000001";
    const std::string config = "{\"target\":\"" + targetGuid + "\",\"targets\":[\"" + targetGuid
        + "\",null,\"" + missingGuid + "\",\"" + targetGuid
        + "\"],\"weights\":[2.5,4.0],\"counts\":[-2,4],\"toggles\":[false,true],"
          "\"labels\":[\"a\",\"b\"],\"offsets\":[[1.0,2.0,3.0]],\"modes\":[1],\"moveCached\":false}";
    ASSERT_TRUE(references->ApplySettingsJson(config)) << runtime.LastDiagnostic();
    auto fields = Fields(*references);
    ASSERT_NE(fields["targets"].as_array(), nullptr);
    const auto& slots = *fields["targets"].as_array();
    ASSERT_EQ(slots.size(), 4u);
    EXPECT_EQ(slots[0].value_or(std::string{}), targetGuid);
    EXPECT_EQ(slots[1].value_or(std::string{"not null"}), "");
    EXPECT_EQ(slots[2].value_or(std::string{}), missingGuid);
    EXPECT_EQ(slots[3].value_or(std::string{}), targetGuid);
    EXPECT_DOUBLE_EQ((*fields["weights"].as_array())[0].value_or(0.0), 2.5);
    EXPECT_EQ((*fields["counts"].as_array())[0].value_or(int64_t{}), -2);
    EXPECT_FALSE((*fields["toggles"].as_array())[0].value_or(true));
    EXPECT_EQ((*fields["labels"].as_array())[1].value_or(std::string{}), "b");
    ASSERT_NE((*fields["offsets"].as_array())[0].as_array(), nullptr);
    EXPECT_DOUBLE_EQ((*(*fields["offsets"].as_array())[0].as_array())[2].value_or(0.0), 3.0);
    EXPECT_EQ((*fields["modes"].as_array())[0].value_or(int64_t{}), 1);

    EXPECT_FALSE(references->ApplySettingsJson("{\"counts\":[99],\"weights\":[2.0,100.0]}"));
    EXPECT_EQ((*Fields(*references)["counts"].as_array())[0].value_or(int64_t{}), -2);
    EXPECT_FALSE(references->ApplySettingsJson("{\"weights\":null}"));
    EXPECT_FALSE(references->ApplySettingsJson("{\"labels\":[null]}"));
    EXPECT_FALSE(references->ApplySettingsJson("{\"modes\":[999]}"));
    Tick();
    EXPECT_VEC3_NEAR(target.transform.position, (math::Vector3{1, 0, 0}), testkit::kTolerance);
    EXPECT_EQ(Fields(*health)["health"].value_or(int64_t{}), 7);
    EXPECT_FALSE(healthEntry.m_awoken);
    EXPECT_TRUE(health->HasLiveContext());
    EXPECT_TRUE(Fields(*references)["cachedTargetAlive"].value_or(false));
    EXPECT_TRUE(Fields(*references)["cachedTransformAlive"].value_or(false));
    EXPECT_TRUE(Fields(*references)["cachedHealthAlive"].value_or(false));

    /// @note Scene 復元は新しい wrapper を作る明示的な再解決。欠損 GUID と null の配列位置は保存のまま。
    const std::string saved = scene::SceneSerializer::SaveToText(m_scene);
    auto restored = scene::SceneSerializer::LoadDataFromText(saved, {});
    ASSERT_NE(restored, nullptr);
    auto* restoredOwner = restored->Find("References");
    ASSERT_NE(restoredOwner, nullptr);
    auto* restoredScript = restoredOwner->GetComponent<scene::ScriptComponent>()->scripts.front().script.get();
    ASSERT_NE(restoredScript, nullptr);
    const auto restoredFields = Fields(*restoredScript);
    ASSERT_EQ(restoredFields["targets"].as_array()->size(), 4u);
    EXPECT_EQ((*restoredFields["targets"].as_array())[2].value_or(std::string{}), missingGuid);
    scene::ManagedBridgeRequest readReference;
    readReference.op = scene::ManagedBridgeOp::READ_FIELD;
    readReference.generation = references->Generation();
    readReference.instanceId = references->InstanceId();
    readReference.input = "target";
    readReference.inputBytes = 6;
    scene::ManagedBridgeReply referenceReply;
    ASSERT_TRUE(runtime.Dispatch(readReference, referenceReply));
    const uint64_t originalObjectId = std::bit_cast<uint64_t>(referenceReply.integer);
    scene::ScriptObjectReference otherSceneReference{targetGuid, scene::EntityID::INVALID};
    auto* restoredAdapter = runtime.FindAdapter(restoredScript);
    ASSERT_NE(restoredAdapter, nullptr);
    EXPECT_TRUE(runtime.ResolveObjectId(*restoredAdapter, originalObjectId, otherSceneReference));
    EXPECT_FALSE(otherSceneReference.id.IsValid());
    restored.reset();

    /// @note Prefab の source GUID は各個体の GUID へ置換され、null/欠損/重複の位置はそのまま。
    ASSERT_TRUE(target.SetParent(&owner));
    const auto prefabPath = m_temp.File("managed-reference.prefab").generic_string();
    ASSERT_TRUE(scene::SceneSerializer::Save(m_scene, prefabPath));
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(scene::InstantiatePrefabAsset(m_scene, prefabPath, roots));
    ASSERT_EQ(roots.size(), 1u);
    auto* prefabOwner = m_scene.GetGameObject(roots.front());
    ASSERT_NE(prefabOwner, nullptr);
    ASSERT_EQ(prefabOwner->GetChildCount(), 1);
    auto* prefabTarget = prefabOwner->GetChild(0);
    ASSERT_NE(prefabTarget, nullptr);
    auto* prefabReferences = prefabOwner->GetComponent<scene::ScriptComponent>()->scripts.front().script.get();
    const auto prefabFields = Fields(*prefabReferences);
    EXPECT_NE(prefabTarget->instanceId, targetGuid);
    EXPECT_EQ(prefabFields["target"].value_or(std::string{}), prefabTarget->instanceId);
    ASSERT_EQ(prefabFields["targets"].as_array()->size(), 4u);
    EXPECT_EQ((*prefabFields["targets"].as_array())[0].value_or(std::string{}), prefabTarget->instanceId);
    EXPECT_EQ((*prefabFields["targets"].as_array())[1].value_or(std::string{"not null"}), "");
    EXPECT_EQ((*prefabFields["targets"].as_array())[2].value_or(std::string{}), missingGuid);
    EXPECT_EQ((*prefabFields["targets"].as_array())[3].value_or(std::string{}), prefabTarget->instanceId);

    /// @note Component を外して同型を追加しても、保持された古い CLR 個体の寿命は復活しない。
    target.GetComponent<scene::ScriptComponent>()->scripts.clear();
    auto* replacementHealth = Attach(target, "cs-internal:ManagedFixtures.HealthComponent");
    ASSERT_NE(replacementHealth, nullptr);
    Tick();
    EXPECT_FALSE(Fields(*references)["cachedHealthAlive"].value_or(true));
    EXPECT_TRUE(Fields(*references)["cachedTargetAlive"].value_or(false));
    EXPECT_EQ(Fields(*replacementHealth)["health"].value_or(int64_t{}), 7);

    /// @note Entity index と GUID を再利用しても既存 token は古い Entity generation を指し続ける。
    ASSERT_TRUE(m_scene.DestroyGameObject(targetEntity));
    auto& reused = m_scene.CreateGameObject("ReusedTarget");
    reused.instanceId = targetGuid;
    EXPECT_EQ(reused.GetID().index, targetEntity.index);
    EXPECT_NE(reused.GetID().generation, targetEntity.generation);
    Tick();
    EXPECT_FALSE(Fields(*references)["cachedTargetAlive"].value_or(true));
    EXPECT_FALSE(Fields(*references)["cachedTransformAlive"].value_or(true));
    EXPECT_VEC3_NEAR(reused.transform.position, math::Vector3::ZERO, testkit::kTolerance);
    EXPECT_FALSE(references->IsRuntimeFaulted());

    /// @note optional 参照は非 null の欠損 wrapper のまま。古い Transform を使うと native 操作は拒否される。
    ASSERT_TRUE(references->ApplySettingsJson("{\"moveCached\":true}"));
    EXPECT_FALSE(references->ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate"));
    EXPECT_TRUE(references->IsRuntimeFaulted());
    EXPECT_VEC3_NEAR(reused.transform.position, math::Vector3::ZERO, testkit::kTolerance);

    /// @note 通常の string[] と反復走査で 128 を超える階層を取得できる。試験用に別世代を増やさない。
    auto& hierarchyRoot = m_scene.CreateGameObject("HierarchyRoot");
    scene::GameObject* leaf = &hierarchyRoot;
    std::string segments = "[";
    for (int index = 0; index < 129; ++index) {
        const std::string name = "Level" + std::to_string(index);
        auto& child = m_scene.CreateGameObject(name);
        ASSERT_TRUE(child.SetParent(leaf));
        leaf = &child;
        if (index) segments += ',';
        segments += '"' + name + '"';
    }
    segments += ']';
    auto& queryOwner = m_scene.CreateGameObject("HierarchyQuery");
    std::string queryType;
    ASSERT_TRUE(scene::ScriptFactory::ResolveTypeName("ManagedFixtures.HierarchyQueryComponent", queryType))
        << "Stage the current managed fixture assembly before running native integration tests.";
    auto* query = Attach(queryOwner, queryType.c_str());
    ASSERT_NE(query, nullptr) << runtime.LastDiagnostic();
    ASSERT_TRUE(query->ApplySettingsJson("{\"root\":\"" + hierarchyRoot.instanceId + "\",\"segments\":" + segments + "}")) << runtime.LastDiagnostic();
    ASSERT_TRUE(query->ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate")) << runtime.LastDiagnostic();
    EXPECT_TRUE(Fields(*query)["succeeded"].value_or(false));
    EXPECT_EQ(Fields(*query)["found"].value_or(std::string{}), leaf->instanceId);
    runtime.Unload(&m_scene);
    EXPECT_TRUE(runtime.Shutdown()) << runtime.LastDiagnostic();
}

TEST_F(ManagedScriptRuntimeTest, RebindsOnlySavedReferencesWhenSceneOwnershipMoves)
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
    ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    auto& target = m_scene.CreateGameObject("MovingTarget");
    ASSERT_NE(Attach(target, "cs-internal:ManagedFixtures.HealthComponent"), nullptr);
    auto& owner = m_scene.CreateGameObject("MovingOwner");
    auto* references = Attach(owner, "cs-internal:ManagedFixtures.ReferenceComponent");
    ASSERT_NE(references, nullptr);
    ASSERT_TRUE(references->ApplySettingsJson("{\"target\":\"" + target.instanceId + "\"}"));
    Tick();
    EXPECT_TRUE(Fields(*references)["cachedTargetAlive"].value_or(false));
    auto serialized = scene::SceneSerializer::SaveToText(m_scene);
    auto loaded = scene::SceneSerializer::LoadDataFromText(serialized, {});
    ASSERT_NE(loaded, nullptr);
    const uint64_t temporaryLife = loaded->GetRenderSceneGeneration();
    m_scene = std::move(*loaded);
    loaded.reset();
    EXPECT_NE(m_scene.GetRenderSceneGeneration(), temporaryLife);
    auto* movedOwner = m_scene.Find("MovingOwner");
    auto* movedTarget = m_scene.Find("MovingTarget");
    ASSERT_NE(movedOwner, nullptr);
    ASSERT_NE(movedTarget, nullptr);
    auto* movedScript = movedOwner->GetComponent<scene::ScriptComponent>()->scripts.front().script.get();
    ASSERT_NE(movedScript, nullptr);
    Tick();
    EXPECT_TRUE(Fields(*movedScript)["cachedTargetAlive"].value_or(false));
    EXPECT_VEC3_NEAR(movedTarget->transform.position, (math::Vector3{2, 0, 0}), testkit::kTolerance);
    EXPECT_FALSE(movedScript->IsRuntimeFaulted());

    /// @note 生きた CLR 個体を move した場合も保存 field だけ再接続し、古い private wrapper は失効する。
    scene::Scene moved = std::move(m_scene);
    auto* movedAgainOwner = moved.Find("MovingOwner");
    auto* movedAgainTarget = moved.Find("MovingTarget");
    ASSERT_NE(movedAgainOwner, nullptr);
    ASSERT_NE(movedAgainTarget, nullptr);
    auto* movedAgainScript = movedAgainOwner->GetComponent<scene::ScriptComponent>()->scripts.front().script.get();
    ASSERT_NE(movedAgainScript, nullptr);
    EXPECT_TRUE(movedAgainScript->ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate"));
    EXPECT_FALSE(Fields(*movedAgainScript)["cachedTargetAlive"].value_or(true));
    EXPECT_FALSE(Fields(*movedAgainScript)["cachedTransformAlive"].value_or(true));
    EXPECT_VEC3_NEAR(movedAgainTarget->transform.position, (math::Vector3{3, 0, 0}), testkit::kTolerance);
    runtime.Unload(&moved);
    EXPECT_TRUE(runtime.Shutdown());
}

TEST_F(ManagedScriptRuntimeTest, ValidatesRequiredReferencesAndKeepsUnknownAndRenamedSavedFields)
{
    auto& runtime = scene::ManagedScriptRuntime::Instance();
    ASSERT_TRUE(runtime.Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
    ASSERT_TRUE(runtime.Load(FBZZ_MANAGED_FIXTURE_TEST_ASSEMBLY, true)) << runtime.LastDiagnostic();
    auto& target = m_scene.CreateGameObject("RequiredTarget");
    auto& requiredOwner = m_scene.CreateGameObject("Required");
    auto* required = Attach(requiredOwner, "cs-internal:ManagedFixtures.RequiredComponent");
    ASSERT_NE(required, nullptr);
    EXPECT_FALSE(required->ValidateSettingsJson("{}"));
    EXPECT_FALSE(required->ApplySettingsJson("{\"updateCount\":9,\"target\":null}"));
    EXPECT_EQ(Fields(*required)["updateCount"].value_or(int64_t{}), 0);
    EXPECT_TRUE(required->RestoreSettingsJson("{\"updateCount\":0,\"target\":null}"));
    ASSERT_TRUE(required->ApplySettingsJson("{\"target\":\"" + target.instanceId + "\"}"));
    EXPECT_TRUE(required->ValidateReferences());
    auto& arrayOwner = m_scene.CreateGameObject("RequiredArray");
    auto* array = Attach(arrayOwner, "cs-internal:ManagedFixtures.RequiredArrayComponent");
    ASSERT_NE(array, nullptr);
    EXPECT_FALSE(array->ApplySettingsJson("{\"targets\":[]}"));
    EXPECT_FALSE(array->ApplySettingsJson("{\"targets\":[null]}"));
    ASSERT_TRUE(array->ApplySettingsJson("{\"targets\":[\"" + target.instanceId + "\"]}"));
    EXPECT_TRUE(array->ValidateReferences());

    auto& settingsOwner = m_scene.CreateGameObject("UnknownSettings");
    auto* settings = Attach(settingsOwner, "cs-internal:ManagedFixtures.SettingsComponent");
    ASSERT_NE(settings, nullptr);
    ASSERT_TRUE(runtime.RestoreSettingsToml(*settings, "oldSpeed = 6.0\nfutureSetting = [10, 20]\n"));
    settingsOwner.GetComponent<scene::ScriptComponent>()->scripts.front().serialized = std::make_unique<scene::SerializedScriptData>(
        scene::SerializedScriptData{settings->GetTypeName(), "oldSpeed = 6.0\nfutureSetting = [10, 20]\n", true});
    EXPECT_DOUBLE_EQ(Fields(*settings)["stableSpeed"].value_or(0.0), 6.0);
    const std::string text = scene::SceneSerializer::SaveToText(m_scene);
    EXPECT_NE(text.find("futureSetting"), std::string::npos);
    EXPECT_EQ(text.find("oldSpeed"), std::string::npos);
    Tick();
    EXPECT_EQ(Fields(*required)["updateCount"].value_or(int64_t{}), 1);
    const scene::EntityID oldTarget = target.GetID();
    ASSERT_TRUE(m_scene.DestroyGameObject(oldTarget));
    EXPECT_FALSE(required->ValidateReferences());
    EXPECT_FALSE(array->ValidateReferences());
    runtime.Unload(&m_scene);
    EXPECT_TRUE(runtime.Shutdown());
}

} /// @note namespace fbzz::tests
