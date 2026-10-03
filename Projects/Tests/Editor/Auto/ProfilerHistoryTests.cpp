/// @file    ProfilerHistoryTests.cpp
/// @brief   注入した確定区間で平均、世代別集計、履歴 Query と不可用値を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Profiler/ProfilerHistory.hpp>
#include <Editor/Profiler/ProfilerOperators.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {
class RestoreProfileProbe final : public scene::Script {
    FBZZ_SCRIPT(RestoreProfileProbe)
};
FBZZ_REFLECT(RestoreProfileProbe)

scene::ScriptProfileSnapshot CollectedFrame(uint64_t serial = 1, uint64_t session = 1)
{
    scene::ScriptProfileSnapshot frame;
    frame.recording = true;
    frame.available = true;
    frame.complete = true;
    frame.captureSessionId = session;
    frame.applicationFrameSerial = serial;
    frame.contexts.push_back({1, 10, 20, 30, scene::ScriptProfileMode::PLAY});
    for (uint64_t key = 1; key <= 2; ++key) {
        scene::ScriptProfileDescriptor descriptor;
        descriptor.sampleKey = key;
        descriptor.executionContextId = 1;
        descriptor.typeId = 7;
        descriptor.callbackKind = scene::ScriptCallbackKind::UPDATE;
        descriptor.typeName = "SameType";
        descriptor.objectName = "SameName";
        descriptor.inspectionId = key == 1 ? "script-a" : "script-b";
        descriptor.instanceId = key == 1 ? "object-a" : "object-b";
        descriptor.callbackLabel = "OnUpdate";
        frame.descriptors.push_back(std::move(descriptor));
    }
    profiler::ProfileSample parent;
    parent.sampleId = 1; parent.sampleKey = 1; parent.inclusiveMs = 5.0; parent.selfMs = 3.0;
    profiler::ProfileSample child;
    child.sampleId = 2; child.sampleKey = 2; child.parentSampleId = 1; child.depth = 1;
    child.startOffsetMs = 1.0; child.inclusiveMs = 2.0; child.selfMs = 2.0;
    frame.samples = {parent, child};
    frame.recordedSampleCount = 2;
    frame.scopeRootSumMs = 5.0;
    return frame;
}

const editor::ScriptProfileRow* FindRow(const std::vector<editor::ScriptProfileRow>& rows, const std::string& instance)
{
    const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.descriptor.inspectionId == instance; });
    return found == rows.end() ? nullptr : &*found;
}
} /// @note namespace

class ProfilerHistoryTest : public testkit::EditorFixture {
protected:
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    bool m_injected = false;
    bool m_wasPerformance = false;
    bool m_wasScript = false;
    bool m_wasPlay = false;
    void SetUp() override
    {
        EditorFixture::SetUp();
        editor::ResetProfilerHistory();
        Context().operators = &m_registry;
        Context().undoStack = &m_undo;
        editor::RegisterProfilerOperators(m_registry);
        m_wasPerformance = profiler::Profiler::IsEnabled();
        m_wasScript = scene::ScriptProfiler::IsRecording();
        m_wasPlay = scene::Script::IsInPlayMode();
        scene::Script::SetInPlayMode(false);
    }
    void TearDown() override
    {
        /// @note 直接注入した値は採取台帳へ加算していないため、台帳を減算する Clear の前に退役する。
        if (m_injected) {
            editor::GetScriptHistory() = {};
            editor::GetPerformanceHistory() = {};
        }
        editor::ResetProfilerHistory();
        profiler::Profiler::EndFrame();
        scene::ScriptProfiler::EndFrame();
        profiler::Profiler::SetEnabled(m_wasPerformance);
        scene::ScriptProfiler::RequestRecording(m_wasScript);
        scene::ScriptProfiler::BeginFrame(99999, profiler::ProfileRecorder::NowMs());
        scene::ScriptProfiler::EndFrame();
        scene::Script::SetInPlayMode(m_wasPlay);
        EditorFixture::TearDown();
    }
    void Inject(scene::ScriptProfileSnapshot frame)
    {
        m_injected = true;
        auto owned = std::make_shared<const scene::ScriptProfileSnapshot>(std::move(frame));
        auto& history = editor::GetScriptHistory();
        history.frames.push_back(owned);
        history.selected = std::move(owned);
    }
    editor::OpArgs FrameArgs(uint64_t serial = 1, uint64_t session = 1) const
    {
        editor::OpArgs args;
        args.Set("captureSessionId", std::to_string(session));
        args.Set("applicationFrameSerial", std::to_string(serial));
        return args;
    }
    editor::OpResult Query(const editor::OpArgs& args = {})
    {
        return editor::InvokeOperator(Context(), "profiler.script.snapshot", args);
    }
};

TEST_F(ProfilerHistoryTest, ComputesNormalSelfAndCallAndFrameAveragesFromParentFiveChildTwo)
{
    const auto frame = CollectedFrame();
    const auto rows = editor::AggregateScriptProfiles({&frame}, editor::ScriptProfileGroup::INSTANCE);
    ASSERT_EQ(rows.size(), 2u);
    const auto* parent = FindRow(rows, "script-a");
    const auto* child = FindRow(rows, "script-b");
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(child, nullptr);
    EXPECT_DOUBLE_EQ(parent->inclusiveMs, 5.0);
    EXPECT_DOUBLE_EQ(parent->selfMs, 3.0);
    EXPECT_DOUBLE_EQ(parent->avgPerCallMs, 5.0);
    EXPECT_DOUBLE_EQ(parent->avgPerFrameMs, 5.0);
    EXPECT_DOUBLE_EQ(child->selfMs, 2.0);
    EXPECT_DOUBLE_EQ(parent->selfMs + child->selfMs, 5.0);
    EXPECT_EQ(parent->calls, 1u);
    EXPECT_EQ(parent->validFrames, 1u);
    EXPECT_DOUBLE_EQ(parent->share, 0.6);
    EXPECT_DOUBLE_EQ(child->share, 0.4);
}

TEST_F(ProfilerHistoryTest, IncludesObservedEmptyFramesAsZeroButDoesNotIncludeUnobservedContext)
{
    const auto frame = CollectedFrame();
    auto empty = CollectedFrame(2);
    empty.samples.clear(); empty.descriptors.clear(); empty.recordedSampleCount = 0;
    auto unobserved = empty;
    unobserved.applicationFrameSerial = 3;
    unobserved.contexts[0].executionContextId = 99;
    const auto rows = editor::AggregateScriptProfiles({&frame, &empty, &unobserved}, editor::ScriptProfileGroup::INSTANCE);
    const auto* parent = FindRow(rows, "script-a");
    ASSERT_NE(parent, nullptr);
    EXPECT_EQ(parent->validFrames, 2u);
    EXPECT_DOUBLE_EQ(parent->avgPerFrameMs, 2.5);
}

TEST_F(ProfilerHistoryTest, ExcludesIncompleteAndTransitionFrameTimesFromNormalAverageAndPeak)
{
    const auto frame = CollectedFrame();
    auto incomplete = CollectedFrame(2);
    incomplete.complete = false;
    incomplete.samples[0].inclusiveMs = 100.0; incomplete.samples[0].selfMs = 80.0;
    auto transition = incomplete;
    transition.applicationFrameSerial = 3; transition.complete = true; transition.transitionFrame = true;
    const auto rows = editor::AggregateScriptProfiles({&frame, &incomplete, &transition}, editor::ScriptProfileGroup::INSTANCE);
    const auto* parent = FindRow(rows, "script-a");
    ASSERT_NE(parent, nullptr);
    EXPECT_EQ(parent->validFrames, 1u);
    EXPECT_EQ(parent->excludedFrames, 2u);
    EXPECT_DOUBLE_EQ(parent->inclusiveMs, 5.0);
    EXPECT_DOUBLE_EQ(parent->avgPerCallMs, 5.0);
    EXPECT_DOUBLE_EQ(parent->avgPerFrameMs, 5.0);
    EXPECT_DOUBLE_EQ(parent->maxCallMs, 5.0);
    EXPECT_DOUBLE_EQ(parent->peakFrameMs, 5.0);
    EXPECT_FALSE(parent->shareAvailable);
}

TEST_F(ProfilerHistoryTest, KeepsFaultDurationsOutsideNormalTotalsAndCountsFaults)
{
    const auto frame = CollectedFrame();
    auto fault = CollectedFrame(2);
    fault.complete = false;
    fault.samples[0].status = profiler::ProfileSampleStatus::FAULTED;
    fault.samples[0].inclusiveMs = 9.0; fault.samples[0].selfAvailable = false;
    fault.samples[1].status = profiler::ProfileSampleStatus::ABORTED;
    const auto rows = editor::AggregateScriptProfiles({&frame, &fault}, editor::ScriptProfileGroup::INSTANCE);
    const auto* parent = FindRow(rows, "script-a");
    ASSERT_NE(parent, nullptr);
    EXPECT_EQ(parent->calls, 2u);
    EXPECT_EQ(parent->faults, 1u);
    EXPECT_DOUBLE_EQ(parent->faultElapsedMs, 9.0);
    EXPECT_DOUBLE_EQ(parent->inclusiveMs, 5.0);
    EXPECT_DOUBLE_EQ(parent->avgPerCallMs, 5.0);
}

TEST_F(ProfilerHistoryTest, SeparatesSameTypeSameNameInstancesContextsAndSessions)
{
    const auto frame = CollectedFrame();
    auto nextSession = CollectedFrame(2, 2);
    auto nextContext = CollectedFrame(3);
    nextContext.contexts[0].executionContextId = 2;
    nextContext.contexts[0].runtimeEpoch = 11;
    for (auto& descriptor : nextContext.descriptors) descriptor.executionContextId = 2;
    const auto instances = editor::AggregateScriptProfiles({&frame}, editor::ScriptProfileGroup::INSTANCE);
    ASSERT_EQ(instances.size(), 2u);
    EXPECT_NE(instances[0].descriptor.inspectionId, instances[1].descriptor.inspectionId);
    const auto types = editor::AggregateScriptProfiles({&frame, &nextSession, &nextContext}, editor::ScriptProfileGroup::TYPE);
    EXPECT_EQ(types.size(), 3u);
    for (const auto& row : types) {
        EXPECT_EQ(row.validFrames, 1u);
        EXPECT_DOUBLE_EQ(row.inclusiveMs, 7.0);
        EXPECT_DOUBLE_EQ(row.selfMs, 5.0);
    }
}

TEST_F(ProfilerHistoryTest, UsesAllScriptSelfBeforeFilteringForShare)
{
    const auto frame = CollectedFrame();
    editor::ScriptProfileFilter filter;
    filter.instance = "script-b";
    const auto rows = editor::AggregateScriptProfiles({&frame}, editor::ScriptProfileGroup::INSTANCE, filter);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].shareAvailable);
    EXPECT_DOUBLE_EQ(rows[0].share, 0.4);
}

TEST_F(ProfilerHistoryTest, MakesShareUnavailableWhenAnotherScriptSelfCannotBeEstablished)
{
    auto frame = CollectedFrame();
    frame.samples[0].selfAvailable = false;
    editor::ScriptProfileFilter filter;
    filter.instance = "script-b";
    const auto rows = editor::AggregateScriptProfiles({&frame}, editor::ScriptProfileGroup::INSTANCE, filter);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].shareAvailable);
}

TEST_F(ProfilerHistoryTest, CalculatesSharesWithinEachContextAndSession)
{
    const auto frame = CollectedFrame();
    auto other = CollectedFrame(2, 2);
    other.contexts[0].executionContextId = 2;
    other.contexts[0].runtimeEpoch = 11;
    for (auto& descriptor : other.descriptors) descriptor.executionContextId = 2;
    const auto rows = editor::AggregateScriptProfiles({&frame, &other}, editor::ScriptProfileGroup::INSTANCE);
    ASSERT_EQ(rows.size(), 4u);
    for (const auto& row : rows) EXPECT_DOUBLE_EQ(row.share, row.descriptor.inspectionId == "script-a" ? 0.6 : 0.4);
}

TEST_F(ProfilerHistoryTest, QueriesInjectedFrameWithValidatedFilterSortGroupsAndLimit)
{
    auto frame = CollectedFrame();
    frame.samples[0].inclusiveMs = 20.0; frame.samples[0].selfMs = 8.0;
    frame.samples[1].inclusiveMs = 12.0; frame.samples[1].selfMs = 12.0;
    Inject(std::move(frame));
    auto args = FrameArgs();
    args.Set("sort", std::string("self"));
    args.Set("limit", 1);
    auto result = Query(args);
    ASSERT_TRUE(result.ok);
    ASSERT_NE(result.data.Find("rows"), nullptr);
    ASSERT_EQ(result.data.Find("rows")->AsArray().size(), 1u);
    EXPECT_EQ(result.data.Find("rows")->AsArray()[0].Find("instance")->AsString(), "script-b");
    EXPECT_TRUE(result.data.Find("truncated")->AsBool());
    EXPECT_EQ(result.data.Find("droppedSampleCount")->AsInt(), 0);
    args.Set("sort", std::string("inclusive"));
    result = Query(args);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.data.Find("rows")->AsArray()[0].Find("instance")->AsString(), "script-a");
    args.Set("groupBy", std::string("type"));
    result = Query(args);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.data.Find("rows")->AsArray().size(), 1u);
    EXPECT_FALSE(result.data.Find("truncated")->AsBool());
    EXPECT_TRUE(result.data.Find("rows")->AsArray()[0].Find("instance")->IsNull());
    args.Set("groupBy", std::string("instance"));
    args.Set("instance", std::string("script-b"));
    args.Set("type", std::string("SameType"));
    args.Set("mode", std::string("play"));
    args.Set("scene", std::string("20"));
    args.Set("callback", std::string("OnUpdate"));
    result = Query(args);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.data.Find("rows")->AsArray().size(), 1u);
    EXPECT_EQ(result.data.Find("rows")->AsArray()[0].Find("instance")->AsString(), "script-b");
}

TEST_F(ProfilerHistoryTest, KeepsAncestorsInsideFilteredCallTreeResponseAndDistinguishesTruncation)
{
    Inject(CollectedFrame());
    auto args = FrameArgs();
    args.Set("groupBy", std::string("call_tree"));
    args.Set("instance", std::string("script-b"));
    args.Set("limit", 2);
    auto result = Query(args);
    ASSERT_TRUE(result.ok);
    const auto& samples = result.data.Find("samples")->AsArray();
    ASSERT_EQ(samples.size(), 2u);
    EXPECT_EQ(samples[0].Find("sampleId")->AsString(), samples[1].Find("parentSampleId")->AsString());
    EXPECT_FALSE(result.data.Find("truncated")->AsBool());
    args.Set("limit", 1);
    result = Query(args);
    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.data.Find("samples")->AsArray().empty());
    EXPECT_TRUE(result.data.Find("truncated")->AsBool());
    EXPECT_EQ(result.data.Find("droppedSampleCount")->AsInt(), 0);
}

TEST_F(ProfilerHistoryTest, RejectsUnknownFiltersAndBadLimitsAndNeverSubstitutesMissingFrames)
{
    Inject(CollectedFrame());
    for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
        {"groupBy", "typo"}, {"sort", "typo"}, {"mode", "typo"}, {"callback", "typo"}, {"scene", "-1"}}) {
        auto args = FrameArgs(); args.Set(name, value);
        const auto result = Query(args);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.errorCode, "BAD_ARG");
    }
    for (const int limit : {0, -1, 8193}) {
        auto args = FrameArgs(); args.Set("limit", limit);
        const auto result = Query(args);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.errorCode, "BAD_ARG");
    }
    editor::OpArgs oneId;
    oneId.Set("captureSessionId", std::string("1"));
    EXPECT_EQ(Query(oneId).errorCode, "BAD_ARG");
    EXPECT_EQ(Query(FrameArgs(404, 404)).errorCode, "NOT_FOUND");
}

TEST_F(ProfilerHistoryTest, DefaultQueryIgnoresFrozenSelectionAndExplicitQueryUsesRetainedFrame)
{
    Inject(CollectedFrame(123));
    editor::GetScriptHistory().frozen = true;
    scene::ScriptProfiler::RequestRecording(true);
    scene::ScriptProfiler::RequestClear();
    scene::ScriptProfiler::BeginFrame(999, profiler::ProfileRecorder::NowMs());
    scene::ScriptProfiler::EndFrame();
    const auto latest = Query();
    ASSERT_TRUE(latest.ok);
    EXPECT_EQ(latest.data.Find("applicationFrameSerial")->AsString(), "999");
    const auto retained = Query(FrameArgs(123));
    ASSERT_TRUE(retained.ok);
    EXPECT_EQ(retained.data.Find("applicationFrameSerial")->AsString(), "123");
    EXPECT_TRUE(editor::GetScriptHistory().frozen);
    EXPECT_EQ(editor::GetScriptHistory().selected->applicationFrameSerial, 123u);
}

TEST_F(ProfilerHistoryTest, SamplesOncePerFrameOutsidePanelsAndContinuesWhileFrozen)
{
    profiler::Profiler::SetEnabled(false);
    profiler::Profiler::SetEnabled(true);
    scene::ScriptProfiler::RequestRecording(true);
    scene::ScriptProfiler::RequestClear();
    scene::Scene scene;
    for (uint64_t serial = 901; serial <= 902; ++serial) {
        const auto time = profiler::ProfileRecorder::NowMs();
        profiler::Profiler::BeginFrame(serial, time);
        scene::ScriptProfiler::BeginFrame(serial, time);
        scene::ScriptProfiler::ObserveScene(scene);
        scene::ScriptProfiler::EndFrame();
        profiler::Profiler::EndFrame();
        editor::TickProfilerHistory(Context());
        editor::TickProfilerHistory(Context());
        if (serial == 901) {
            editor::GetScriptHistory().frozen = true;
            editor::GetPerformanceHistory().frozen = true;
        }
    }
    EXPECT_EQ(editor::GetScriptHistory().frames.size(), 2u);
    EXPECT_EQ(editor::GetPerformanceHistory().frames.size(), 2u);
    EXPECT_EQ(editor::GetScriptHistory().selected->applicationFrameSerial, 901u);
    EXPECT_EQ(editor::GetScriptHistory().frames.back()->applicationFrameSerial, 902u);
    EXPECT_EQ(editor::GetPerformanceHistory().selected->applicationFrameSerial, 901u);
}

TEST_F(ProfilerHistoryTest, RecordingActionAppliesAtNextBoundaryWithoutCreatingUndoHistory)
{
    scene::ScriptProfiler::RequestRecording(false);
    scene::ScriptProfiler::BeginFrame(1, profiler::ProfileRecorder::NowMs());
    scene::ScriptProfiler::EndFrame();
    editor::OpArgs args;
    args.Set("recording", true);
    const auto result = editor::InvokeOperator(Context(), "profiler.script.set_recording", args);
    ASSERT_TRUE(result.ok);
    EXPECT_FALSE(scene::ScriptProfiler::IsRecording());
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    scene::ScriptProfiler::BeginFrame(2, profiler::ProfileRecorder::NowMs());
    EXPECT_TRUE(scene::ScriptProfiler::IsRecording());
    scene::ScriptProfiler::EndFrame();
}

TEST_F(ProfilerHistoryTest, GroupsKnownCallbacksByKindAndKeepsUnknownAndEventLabelsDistinct)
{
    auto frame = CollectedFrame();
    frame.descriptors[1].callbackLabel = "module-update-diagnostic";
    auto known = editor::AggregateScriptProfiles({&frame}, editor::ScriptProfileGroup::CALLBACK_KIND);
    ASSERT_EQ(known.size(), 1u);
    EXPECT_EQ(known[0].calls, 2u);
    EXPECT_DOUBLE_EQ(known[0].inclusiveMs, 7.0);
    EXPECT_DOUBLE_EQ(known[0].selfMs, 5.0);
    EXPECT_EQ(known[0].descriptor.callbackKind, scene::ScriptCallbackKind::UPDATE);
    for (auto& descriptor : frame.descriptors) descriptor.inspectionId = "same-instance";
    const auto knownInstances = editor::AggregateScriptProfiles({&frame}, editor::ScriptProfileGroup::INSTANCE);
    ASSERT_EQ(knownInstances.size(), 1u);
    EXPECT_EQ(knownInstances[0].calls, 2u);
    for (const auto kind : {scene::ScriptCallbackKind::UNKNOWN, scene::ScriptCallbackKind::EVENT_HANDLER}) {
        for (auto& descriptor : frame.descriptors) {
            descriptor.callbackKind = kind;
            descriptor.inspectionId = "same-instance";
        }
        const auto callbacks = editor::AggregateScriptProfiles({&frame}, editor::ScriptProfileGroup::CALLBACK_KIND);
        ASSERT_EQ(callbacks.size(), 2u);
        EXPECT_NE(callbacks[0].descriptor.callbackLabel, callbacks[1].descriptor.callbackLabel);
        const auto instances = editor::AggregateScriptProfiles({&frame}, editor::ScriptProfileGroup::INSTANCE);
        ASSERT_EQ(instances.size(), 2u);
        EXPECT_NE(instances[0].descriptor.callbackLabel, instances[1].descriptor.callbackLabel);
    }
}

TEST_F(ProfilerHistoryTest, KeepsFrozenFrameInside240FrameRingWhileCollectingNewFrames)
{
    profiler::Profiler::SetEnabled(false);
    profiler::Profiler::SetEnabled(true);
    scene::ScriptProfiler::RequestRecording(true);
    scene::ScriptProfiler::RequestClear();
    scene::Scene scene;
    std::shared_ptr<const scene::ScriptProfileSnapshot> pinnedScript;
    std::shared_ptr<const editor::PerformanceCapture> pinnedPerformance;
    for (uint64_t serial = 1; serial <= 245; ++serial) {
        const auto time = profiler::ProfileRecorder::NowMs();
        profiler::Profiler::BeginFrame(serial, time);
        scene::ScriptProfiler::BeginFrame(serial, time);
        scene::ScriptProfiler::ObserveScene(scene);
        scene::ScriptProfiler::EndFrame();
        profiler::Profiler::EndFrame();
        editor::TickProfilerHistory(Context());
        editor::TickProfilerHistory(Context());
        if (serial == 1) {
            pinnedScript = editor::GetScriptHistory().selected;
            pinnedPerformance = editor::GetPerformanceHistory().selected;
            editor::GetScriptHistory().frozen = true;
            editor::GetPerformanceHistory().frozen = true;
        }
        const size_t expected = static_cast<size_t>((std::min)(serial, uint64_t{240}));
        EXPECT_EQ(editor::GetScriptHistory().frames.size(), expected);
        EXPECT_EQ(editor::GetPerformanceHistory().frames.size(), expected);
        if (serial == 240) {
            EXPECT_EQ(editor::GetScriptHistory().historyEvictedFrameCount, 0u);
            EXPECT_EQ(editor::GetPerformanceHistory().historyEvictedFrameCount, 0u);
        }
    }
    const auto& script = editor::GetScriptHistory();
    const auto& performance = editor::GetPerformanceHistory();
    EXPECT_EQ(script.selected, pinnedScript);
    EXPECT_EQ(performance.selected, pinnedPerformance);
    EXPECT_EQ(script.frames.front(), pinnedScript);
    EXPECT_EQ(performance.frames.front(), pinnedPerformance);
    EXPECT_EQ(script.frames[1]->applicationFrameSerial, 7u);
    EXPECT_EQ(performance.frames[1]->applicationFrameSerial, 7u);
    EXPECT_EQ(script.frames.back()->applicationFrameSerial, 245u);
    EXPECT_EQ(performance.frames.back()->applicationFrameSerial, 245u);
    EXPECT_EQ(script.historyEvictedFrameCount, 5u);
    EXPECT_EQ(performance.historyEvictedFrameCount, 5u);
    EXPECT_FALSE(script.budgetBlocked);
    EXPECT_FALSE(performance.budgetBlocked);
    const auto latest = Query();
    ASSERT_TRUE(latest.ok);
    EXPECT_EQ(latest.data.Find("applicationFrameSerial")->AsString(), "245");
    EXPECT_EQ(latest.data.Find("retainedFrameCount")->AsInt(), 240);
    const auto retained = Query(FrameArgs(1, pinnedScript->captureSessionId));
    ASSERT_TRUE(retained.ok);
    EXPECT_EQ(retained.data.Find("applicationFrameSerial")->AsString(), "1");
    EXPECT_EQ(Query(FrameArgs(2, pinnedScript->captureSessionId)).errorCode, "NOT_FOUND");
}

TEST_F(ProfilerHistoryTest, DataRestoreKeepsNewAuthoringCallbacksEditAndOldDestroyPlay)
{
    ASSERT_TRUE(scene::ScriptFactory::Register<RestoreProfileProbe>());
    profiler::Profiler::SetEnabled(false);
    scene::ScriptProfiler::RequestRecording(true);
    scene::ScriptProfiler::RequestClear();
    scene::Script::SetInPlayMode(true);
    auto& oldScene = AttachScene();
    auto& oldObject = oldScene.CreateGameObject("Authoring");
    auto& oldScript = oldObject.AddScript<RestoreProfileProbe>();
    ASSERT_TRUE(oldScript.ExecuteProfiledCallback(&scene::Script::OnAwake, scene::ScriptCallbackKind::AWAKE, "OnAwake"));
    auto* oldComponent = oldObject.GetComponent<scene::ScriptComponent>();
    ASSERT_NE(oldComponent, nullptr);
    oldComponent->scripts[0].m_awoken = true;
    const std::string oldInspectionId = oldScript.InspectionId();
    const auto text = scene::SceneSerializer::SaveToText(oldScene);
    ASSERT_FALSE(text.empty());
    scene::ScriptProfiler::BeginFrame(201, profiler::ProfileRecorder::NowMs());
    /// @note Play 停止と同じ復元順序をデータ専用 loader で検証し、Application/描画デバイスを要求しない。
    scene::ScriptProfiler::SetExecutionMode(false);
    auto restored = scene::SceneSerializer::LoadDataFromText(text, File("authoring.scene").generic_string());
    ASSERT_NE(restored, nullptr);
    oldScene = std::move(*restored);
    scene::ScriptProfiler::EndFrame();
    const auto& frame = scene::ScriptProfiler::GetSnapshot();
    EXPECT_TRUE(frame.transitionFrame);
    bool sawAfterDeserialize = false;
    bool sawValidate = false;
    bool sawOldDestroy = false;
    for (const auto& descriptor : frame.descriptors) {
        if (descriptor.typeName != "RestoreProfileProbe") continue;
        const auto* context = editor::FindScriptContext(frame, descriptor.executionContextId);
        ASSERT_NE(context, nullptr);
        if (descriptor.callbackKind == scene::ScriptCallbackKind::DESTROY) {
            sawOldDestroy = true;
            EXPECT_EQ(context->mode, scene::ScriptProfileMode::PLAY);
            EXPECT_EQ(descriptor.inspectionId, oldInspectionId);
        } else if (descriptor.callbackKind == scene::ScriptCallbackKind::AFTER_DESERIALIZE ||
                   descriptor.callbackKind == scene::ScriptCallbackKind::VALIDATE) {
            EXPECT_EQ(context->mode, scene::ScriptProfileMode::EDIT);
            EXPECT_NE(descriptor.inspectionId, oldInspectionId);
            sawAfterDeserialize = sawAfterDeserialize || descriptor.callbackKind == scene::ScriptCallbackKind::AFTER_DESERIALIZE;
            sawValidate = sawValidate || descriptor.callbackKind == scene::ScriptCallbackKind::VALIDATE;
        }
    }
    EXPECT_TRUE(sawAfterDeserialize);
    EXPECT_TRUE(sawValidate);
    EXPECT_TRUE(sawOldDestroy);
    const auto rows = editor::AggregateScriptProfiles({&frame}, editor::ScriptProfileGroup::TYPE);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_NE(rows[0].context.mode, rows[1].context.mode);
}

} /// @note namespace fbzz::tests
