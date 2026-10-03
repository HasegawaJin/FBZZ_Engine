/// @file    ScriptProfilerTests.cpp
/// @brief   Script 個体、入口、文脈、履歴寿命と SEH 復旧を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Core/Profiler/Profiler.hpp>
#include <Engine/Profiler/ScriptProfiler.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptModules.hpp>
#include <Engine/Scene/ScriptProxy/ScriptTweenProxy.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Physics/World.hpp>
#include <Engine/Scene/ScriptEvent.hpp>
#include <algorithm>
#if defined(_MSC_VER)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace fbzz::tests {
namespace {
class ProfileProbe : public scene::Script {
public:
    const char* GetTypeName() const override { return "ProfileProbe"; }
    scene::Script* child = nullptr;
    bool publish = false;
    bool fault = false;
    bool editMode = false;
    bool ExecuteInEditMode() const override { return editMode; }
    void CustomCallback() { ++updates; }
    int updates = 0;
    void OnUpdate() override
    {
        ++updates;
        if (child) child->ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate");
        if (publish) scene::ScriptEventBus::PublishRaw("probe", nullptr);
#if defined(_MSC_VER)
        if (fault) {
            profiler::Profiler::BeginSample(profiler::ProfilerMarker("FaultedEngineScope"));
            /// @see https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-raiseexception RaiseException の SEH ディスパッチ契約。
            RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        }
#endif
    }
};

class ModuleOwnerProbe : public scene::Script {
public:
    ProfileProbe module;
    scene::ScriptModules modules{*this, {scene::ScriptModule(module).Update().LateUpdate().FixedUpdate()}};
    const char* GetTypeName() const override { return "ModuleOwnerProbe"; }
    void OnStart() override { modules.Start(); }
    void OnUpdate() override { modules.Update(0.0f); }
    void OnLateUpdate() override { modules.LateUpdate(); }
    void OnFixedUpdate() override { modules.FixedUpdate(); }
    void OnDestroy() override { modules.Destroy(); }
    void OnDrawGizmos() override { modules.DrawGizmos(); }
};

} /// @note namespace

class ScriptProfilerTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;
    physics::World m_world;
    bool m_wasPerformance = false;
    bool m_wasScript = false;
    bool m_wasPlay = false;
    uint64_t m_serial = 0;
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_wasPerformance = profiler::Profiler::IsEnabled();
        m_wasScript = scene::ScriptProfiler::IsRecording();
        m_wasPlay = scene::Script::IsInPlayMode();
        profiler::Profiler::SetEnabled(false);
        profiler::Profiler::SetEnabled(true);
        scene::Script::SetInPlayMode(false);
        scene::ScriptProfiler::RequestRecording(true);
        scene::ScriptProfiler::RequestClear();
    }
    void TearDown() override
    {
        scene::ScriptProfiler::EndFrame();
        profiler::Profiler::EndFrame();
        m_scene.Clear();
        scene::ScriptEventBus::Clear();
        scene::ScriptProfiler::RequestRecording(m_wasScript);
        scene::ScriptProfiler::RequestClear();
        scene::ScriptProfiler::BeginFrame(++m_serial, profiler::ProfileRecorder::NowMs());
        scene::ScriptProfiler::EndFrame();
        scene::Script::SetInPlayMode(m_wasPlay);
        profiler::Profiler::SetEnabled(m_wasPerformance);
        EngineFixture::TearDown();
    }
    ProfileProbe& Add(const char* name = "same")
    {
        auto& go = m_scene.CreateGameObject(name);
        auto& script = go.AddScript<ProfileProbe>();
        script.SetContext(&m_scene, &go);
        return script;
    }
    void Begin()
    {
        const double time = profiler::ProfileRecorder::NowMs();
        profiler::Profiler::BeginFrame(++m_serial, time);
        scene::ScriptProfiler::BeginFrame(m_serial, time);
        scene::ScriptProfiler::ObserveScene(m_scene);
    }
    void Tick(bool playMode)
    {
        SystemContext context{m_scene, m_world, nullptr, nullptr, 0.016f, 0.02f, playMode, playMode};
        scene::ScriptSystem{}.Update(context);
    }
    void End()
    {
        scene::ScriptProfiler::EndFrame();
        profiler::Profiler::EndFrame();
    }
};

TEST_F(ScriptProfilerTest, SeparatesSameTypeSameNameInstancesAndNestedCalls)
{
    auto& first = Add();
    auto& second = Add();
    first.child = &second;
    Begin();
    ASSERT_TRUE(first.ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate"));
    End();
    const auto& snapshot = scene::ScriptProfiler::GetSnapshot();
    ASSERT_EQ(snapshot.samples.size(), 2u);
    ASSERT_EQ(snapshot.descriptors.size(), 2u);
    EXPECT_NE(snapshot.descriptors[0].inspectionId, snapshot.descriptors[1].inspectionId);
    EXPECT_NE(snapshot.descriptors[0].instanceId, snapshot.descriptors[1].instanceId);
    EXPECT_EQ(snapshot.descriptors[0].typeId, snapshot.descriptors[1].typeId);
    EXPECT_EQ(snapshot.samples[1].parentSampleId, snapshot.samples[0].sampleId);
    EXPECT_EQ(snapshot.descriptors[0].callbackKind, scene::ScriptCallbackKind::UPDATE);
    ASSERT_EQ(snapshot.contexts.size(), 1u);
    EXPECT_EQ(scene::ScriptProfiler::ResolveTarget(m_scene, snapshot.descriptors[0], snapshot.contexts[0]), &first);
}

TEST_F(ScriptProfilerTest, CountsEmptyFixedWaitingAndCompletedCoroutineAtTheirActualEntrances)
{
    auto& script = Add();
    int applications = 0;
    scene::ScriptTweenProxy tween{&script};
    /// @note 公開 Tween API の生成フレーム待機を使い、時計を進めず待機のみの Step を検証する。
    auto waiting = tween.Value(0.0f, 1.0f, 1.0f, [&](float) { ++applications; });
    scene::Coroutine completed;
    ASSERT_FALSE(waiting.Done());
    ASSERT_TRUE(completed.Done());
    EXPECT_FALSE(completed.Step());
    Begin();
    script.ExecuteProfiledCallback(&scene::Script::OnFixedUpdate, scene::ScriptCallbackKind::FIXED_UPDATE, "OnFixedUpdate");
    script.ExecuteProfiledCallback(&scene::Script::OnFixedUpdate, scene::ScriptCallbackKind::FIXED_UPDATE, "OnFixedUpdate");
    script.ExecuteProfiledCallback(&scene::Script::OnAwake, scene::ScriptCallbackKind::AWAKE, "OnAwake");
    EXPECT_TRUE(script.ResumeCoroutine(waiting));
    /// @note Step の false は完了であり、障害隔離入口は正常成功として記録する。
    EXPECT_TRUE(script.ResumeCoroutine(completed));
    End();
    const auto& snapshot = scene::ScriptProfiler::GetSnapshot();
    ASSERT_EQ(snapshot.samples.size(), 5u);
    EXPECT_EQ(applications, 0);
    EXPECT_FALSE(waiting.Done());
    EXPECT_TRUE(completed.Done());
    EXPECT_FALSE(script.IsRuntimeFaulted());
    EXPECT_EQ(std::count_if(snapshot.descriptors.begin(), snapshot.descriptors.end(), [](const auto& descriptor) {
        return descriptor.callbackKind == scene::ScriptCallbackKind::FIXED_UPDATE;
    }), 1);
    EXPECT_EQ(snapshot.descriptors.back().callbackKind, scene::ScriptCallbackKind::COROUTINE_STEP);
    EXPECT_EQ(snapshot.samples[3].sampleKey, snapshot.samples[4].sampleKey);
}

TEST_F(ScriptProfilerTest, CountsDeferredAndFrameDelayWithExplicitKinds)
{
    auto& script = Add();
    int calls = 0;
    const auto handle = script.Invoke([&] { ++calls; }, 0.0f);
    ASSERT_TRUE(handle.IsValid());
    script.FrameDelay(0, [&] { ++calls; });
    Begin();
    script.UpdateInvocations(0.0f);
    script.UpdateFrameDelays();
    End();
    EXPECT_EQ(calls, 2);
    const auto& snapshot = scene::ScriptProfiler::GetSnapshot();
    ASSERT_EQ(snapshot.samples.size(), 2u);
    EXPECT_EQ(snapshot.descriptors[0].callbackKind, scene::ScriptCallbackKind::DEFERRED);
    EXPECT_EQ(snapshot.descriptors[1].callbackKind, scene::ScriptCallbackKind::FRAME_DELAY);
}

TEST_F(ScriptProfilerTest, CountsOwnedEventOnceAndDoesNotAttributeOwnerlessHandler)
{
    auto& sender = Add();
    auto& receiver = Add();
    sender.publish = true;
    int ownedCalls = 0;
    int freeCalls = 0;
    scene::ScriptEventBus::SubscribeRaw(&receiver, "probe", [&](const void*) { ++ownedCalls; });
    scene::ScriptEventBus::SubscribeRaw(nullptr, "probe", [&](const void*) { ++freeCalls; });
    Begin();
    sender.ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate");
    End();
    const auto& snapshot = scene::ScriptProfiler::GetSnapshot();
    EXPECT_EQ(ownedCalls, 1);
    EXPECT_EQ(freeCalls, 1);
    ASSERT_EQ(snapshot.samples.size(), 2u);
    ASSERT_EQ(snapshot.descriptors.size(), 2u);
    EXPECT_EQ(snapshot.descriptors[1].callbackKind, scene::ScriptCallbackKind::EVENT_HANDLER);
    EXPECT_EQ(snapshot.descriptors[1].callbackLabel, "probe");
    EXPECT_EQ(snapshot.descriptors[1].inspectionId, receiver.InspectionId());
    EXPECT_EQ(snapshot.samples[1].parentSampleId, snapshot.samples[0].sampleId);
}

TEST_F(ScriptProfilerTest, PreservesOldAndNewContextsInOneTransitionFrameAndOwnedHistory)
{
    auto& old = Add();
    Begin();
    old.ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate");
    scene::ScriptProfiler::AdvanceRuntimeEpoch(true);
    old.ExecuteProfiledCallback(&scene::Script::OnDestroy, scene::ScriptCallbackKind::DESTROY, "OnDestroy");
    const auto oldInspection = old.InspectionId();
    auto& fresh = Add();
    fresh.ExecuteProfiledCallback(&scene::Script::OnAwake, scene::ScriptCallbackKind::AWAKE, "OnAwake");
    End();
    const auto retained = scene::ScriptProfiler::GetSnapshot();
    ASSERT_TRUE(retained.transitionFrame);
    ASSERT_EQ(retained.contexts.size(), 2u);
    ASSERT_EQ(retained.descriptors.size(), 3u);
    EXPECT_EQ(retained.descriptors[0].executionContextId, retained.descriptors[1].executionContextId);
    EXPECT_NE(retained.descriptors[1].executionContextId, retained.descriptors[2].executionContextId);
    m_scene.Clear();
    EXPECT_EQ(retained.descriptors[0].typeName, "ProfileProbe");
    EXPECT_EQ(retained.descriptors[0].inspectionId, oldInspection);
    EXPECT_EQ(scene::ScriptProfiler::ResolveTarget(m_scene, retained.descriptors[0], retained.contexts[0]), nullptr);
}

TEST_F(ScriptProfilerTest, KeepsSceneContextForObservedEmptyCollectedFrame)
{
    Begin();
    End();
    const auto& snapshot = scene::ScriptProfiler::GetSnapshot();
    EXPECT_TRUE(snapshot.available);
    EXPECT_TRUE(snapshot.complete);
    EXPECT_TRUE(snapshot.samples.empty());
    ASSERT_EQ(snapshot.contexts.size(), 1u);
    EXPECT_EQ(snapshot.contexts[0].sceneGeneration, m_scene.GetRenderSceneGeneration());
}

TEST_F(ScriptProfilerTest, KeepsPerformanceAndScriptRecordingIndependent)
{
    auto& script = Add();
    for (int performance = 0; performance < 2; ++performance) {
        for (int detail = 0; detail < 2; ++detail) {
            profiler::Profiler::SetEnabled(performance != 0);
            scene::ScriptProfiler::RequestRecording(detail != 0);
            Begin();
            profiler::Profiler::BeginSample(profiler::ProfilerMarker("ScriptSystem"));
            script.ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate");
            profiler::Profiler::EndSample();
            End();
            EXPECT_EQ(profiler::Profiler::IsEnabled(), performance != 0);
            EXPECT_EQ(scene::ScriptProfiler::IsRecording(), detail != 0);
            EXPECT_EQ(profiler::Profiler::GetLastFrameRecords().size(), performance ? 1u : 0u);
            EXPECT_EQ(scene::ScriptProfiler::GetSnapshot().available, detail != 0);
        }
    }
}


TEST_F(ScriptProfilerTest, LegacyUnknownMemberAndFunctionLabelsDoNotGuessKnownKinds)
{
    auto& script = Add();
    Begin();
    EXPECT_TRUE(script.ExecuteCallback(static_cast<void (scene::Script::*)()>(&ProfileProbe::CustomCallback), "OnUpdate"));
    EXPECT_TRUE(script.ExecuteCallback([&] { ++script.updates; }, "OnFixedUpdate"));
    End();
    const auto& snapshot = scene::ScriptProfiler::GetSnapshot();
    ASSERT_EQ(snapshot.descriptors.size(), 2u);
    EXPECT_EQ(script.updates, 2);
    for (const auto& descriptor : snapshot.descriptors) EXPECT_EQ(descriptor.callbackKind, scene::ScriptCallbackKind::UNKNOWN);
}

TEST_F(ScriptProfilerTest, FirstPlayLifecycleRebindsPreviouslyUnawokenEditInstance)
{
    auto& script = Add();
    Tick(false);
    Begin();
    Tick(true);
    End();
    const auto& snapshot = scene::ScriptProfiler::GetSnapshot();
    ASSERT_GE(snapshot.samples.size(), 4u);
    EXPECT_TRUE(snapshot.transitionFrame);
    bool update = false;
    for (const auto& descriptor : snapshot.descriptors) {
        const auto context = std::find_if(snapshot.contexts.begin(), snapshot.contexts.end(), [&](const auto& item) {
            return item.executionContextId == descriptor.executionContextId;
        });
        ASSERT_NE(context, snapshot.contexts.end());
        EXPECT_EQ(context->mode, scene::ScriptProfileMode::PLAY);
        EXPECT_EQ(context->runtimeEpoch, scene::ScriptProfiler::GetRuntimeEpoch());
        update = update || descriptor.callbackKind == scene::ScriptCallbackKind::UPDATE;
    }
    EXPECT_TRUE(update);
    EXPECT_EQ(script.updates, 1);
}

TEST_F(ScriptProfilerTest, RestoredEditLifecycleRebindsInstanceConstructedWhilePlayModeWasActive)
{
    scene::Script::SetInPlayMode(true);
    auto& script = Add();
    script.editMode = true;
    Begin();
    Tick(false);
    End();
    const auto& snapshot = scene::ScriptProfiler::GetSnapshot();
    ASSERT_GE(snapshot.samples.size(), 4u);
    for (const auto& descriptor : snapshot.descriptors) {
        const auto context = std::find_if(snapshot.contexts.begin(), snapshot.contexts.end(), [&](const auto& item) {
            return item.executionContextId == descriptor.executionContextId;
        });
        ASSERT_NE(context, snapshot.contexts.end());
        EXPECT_EQ(context->mode, scene::ScriptProfileMode::EDIT);
        EXPECT_EQ(context->runtimeEpoch, scene::ScriptProfiler::GetRuntimeEpoch());
    }
}

TEST_F(ScriptProfilerTest, InternalModulesKeepExplicitKindsAndOwnIdentityAcrossFirstPlay)
{
    auto& go = m_scene.CreateGameObject("owner");
    auto& owner = go.AddScript<ModuleOwnerProbe>();
    owner.SetContext(&m_scene, &go);
    owner.modules.DrawGizmos();
    Begin();
    Tick(true);
    owner.modules.FixedUpdate();
    owner.modules.LateUpdate();
    End();
    const auto& snapshot = scene::ScriptProfiler::GetSnapshot();
    bool awake = false;
    bool update = false;
    bool fixed = false;
    bool late = false;
    for (const auto& descriptor : snapshot.descriptors) {
        if (descriptor.inspectionId != owner.module.InspectionId()) continue;
        const auto context = std::find_if(snapshot.contexts.begin(), snapshot.contexts.end(), [&](const auto& item) {
            return item.executionContextId == descriptor.executionContextId;
        });
        ASSERT_NE(context, snapshot.contexts.end());
        EXPECT_EQ(context->mode, scene::ScriptProfileMode::PLAY);
        EXPECT_EQ(descriptor.instanceId, go.instanceId);
        EXPECT_NE(descriptor.inspectionId, owner.InspectionId());
        awake = awake || descriptor.callbackKind == scene::ScriptCallbackKind::AWAKE;
        update = update || descriptor.callbackKind == scene::ScriptCallbackKind::UPDATE;
        fixed = fixed || descriptor.callbackKind == scene::ScriptCallbackKind::FIXED_UPDATE;
        late = late || descriptor.callbackKind == scene::ScriptCallbackKind::LATE_UPDATE;
        EXPECT_EQ(scene::ScriptProfiler::ResolveTarget(m_scene, descriptor, *context), &owner.module);
    }
    EXPECT_TRUE(awake);
    EXPECT_TRUE(update);
    EXPECT_TRUE(fixed);
    EXPECT_TRUE(late);
}

TEST_F(ScriptProfilerTest, OwnedEventChannelLabelSurvivesSubscriptionsThatRehashChannels)
{
    auto& receiver = Add();
    int calls = 0;
    scene::ScriptEventBus::SubscribeRaw(&receiver, "probe", [&](const void*) {
        ++calls;
        for (int channel = 0; channel < 512; ++channel)
            scene::ScriptEventBus::SubscribeRaw(nullptr, "rehash-" + std::to_string(channel), [](const void*) {});
    });
    scene::ScriptEventBus::SubscribeRaw(&receiver, "probe", [&](const void*) { ++calls; });
    Begin();
    scene::ScriptEventBus::PublishRaw("probe", nullptr);
    End();
    const auto& snapshot = scene::ScriptProfiler::GetSnapshot();
    EXPECT_EQ(calls, 2);
    ASSERT_EQ(snapshot.samples.size(), 2u);
    ASSERT_EQ(snapshot.descriptors.size(), 1u);
    EXPECT_EQ(snapshot.descriptors[0].callbackKind, scene::ScriptCallbackKind::EVENT_HANDLER);
    EXPECT_EQ(snapshot.descriptors[0].callbackLabel, "probe");
}

#if defined(_MSC_VER)
TEST_F(ScriptProfilerTest, RecoversPerformanceInnerScopeWithDetailEnabledOrDisabled)
{
    for (int detail = 0; detail < 2; ++detail) {
        auto& script = Add();
        script.fault = true;
        scene::ScriptProfiler::RequestRecording(detail != 0);
        Begin();
        const auto outer = profiler::Profiler::BeginScope(profiler::ProfilerMarker("outer"));
        EXPECT_FALSE(script.ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate"));
        profiler::Profiler::EndScope(outer);
        End();
        const auto& performance = profiler::Profiler::GetSnapshot();
        ASSERT_EQ(performance.samples.size(), 2u);
        EXPECT_EQ(performance.samples[0].status, profiler::ProfileSampleStatus::COMPLETE);
        EXPECT_EQ(performance.samples[1].status, profiler::ProfileSampleStatus::ABORTED);
        if (detail) {
            ASSERT_EQ(scene::ScriptProfiler::GetSnapshot().samples.size(), 1u);
            EXPECT_EQ(scene::ScriptProfiler::GetSnapshot().samples[0].status, profiler::ProfileSampleStatus::FAULTED);
        }
        Begin();
        profiler::Profiler::PushMarker(profiler::ProfilerMarker("healthy"));
        End();
        EXPECT_TRUE(profiler::Profiler::GetSnapshot().complete);
        EXPECT_FALSE(script.enabled);
    }
}

TEST_F(ScriptProfilerTest, AttributesOwnedEventFaultToReceiverAndPreservesSender)
{
    auto& sender = Add();
    auto& receiver = Add();
    sender.publish = true;
    scene::ScriptEventBus::SubscribeRaw(&receiver, "probe", [](const void*) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    });
    Begin();
    EXPECT_TRUE(sender.ExecuteProfiledCallback(&scene::Script::OnUpdate, scene::ScriptCallbackKind::UPDATE, "OnUpdate"));
    End();
    EXPECT_TRUE(sender.enabled);
    EXPECT_FALSE(receiver.enabled);
    const auto& snapshot = scene::ScriptProfiler::GetSnapshot();
    ASSERT_EQ(snapshot.samples.size(), 2u);
    EXPECT_EQ(snapshot.samples[0].status, profiler::ProfileSampleStatus::COMPLETE);
    EXPECT_EQ(snapshot.samples[1].status, profiler::ProfileSampleStatus::FAULTED);
}
#endif

TEST_F(ScriptProfilerTest, BoundsOwnedDescriptorStringCapacitiesAndRetiresUnusedLabels)
{
    auto& script = Add();
    const std::function<void()> callback = [] {};
    std::string first;
    Begin();
    for (size_t i = 0; i < profiler::ProfileRecorder::MAX_SAMPLES; ++i) {
        std::string label(129, 'e');
        const auto suffix = std::to_string(i);
        label.replace(0, suffix.size(), suffix);
        if (i == 0) first = label;
        EXPECT_TRUE(script.ExecuteProfiledCallback(callback, scene::ScriptCallbackKind::EVENT_HANDLER, label.c_str()));
    }
    End();
    size_t ownedBytes = 0;
    for (const auto& descriptor : scene::ScriptProfiler::GetSnapshot().descriptors)
        ownedBytes += descriptor.inspectionId.capacity() + 1 + descriptor.instanceId.capacity() + 1 +
            descriptor.objectName.capacity() + 1 + descriptor.typeName.capacity() + 1 + descriptor.callbackLabel.capacity() + 1;
    EXPECT_LE(ownedBytes, size_t{1024 * 1024});
    EXPECT_GT(scene::ScriptProfiler::GetSnapshot().droppedSampleCount, 0u);
    Begin();
    EXPECT_TRUE(script.ExecuteProfiledCallback(callback, scene::ScriptCallbackKind::EVENT_HANDLER, first.c_str()));
    End();
    EXPECT_EQ(scene::ScriptProfiler::GetSnapshot().descriptors.size(), 1u);
    Begin();
    const std::string large(256 * 1024, 'L');
    EXPECT_TRUE(script.ExecuteProfiledCallback(callback, scene::ScriptCallbackKind::EVENT_HANDLER, large.c_str()));
    End();
    EXPECT_EQ(scene::ScriptProfiler::GetSnapshot().droppedSampleCount, 0u);
    EXPECT_EQ(scene::ScriptProfiler::GetSnapshot().descriptors.size(), 1u);
}

} /// @note namespace fbzz::tests
