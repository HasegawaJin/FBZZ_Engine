/// @file    ProfilerBenchmarkTests.cpp
/// @brief   完了 frame の有界 scalar 採取と window/percentile/operator 契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Profiler/ProfilerHistory.hpp>
#include <Editor/Profiler/ProfilerOperators.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <limits>
#include <type_traits>
#include <Profiler/AnalysisProfilerCpu.hpp>
#include <cmath>

namespace fbzz::tests {

class ProfilerBenchmarkTest : public testkit::EditorFixture {
protected:
    editor::PerformanceBenchmarkCapture m_capture;
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    profiler::PerformanceCaptureFilter m_previousFilter;
    bool m_previousRecording = true;
    bool m_previousRecordingRequested = true;

    void SetUp() override
    {
        EditorFixture::SetUp();
        m_previousFilter = profiler::Profiler::GetRequestedCaptureFilter();
        profiler::Profiler::RequestCaptureFilter({});
        m_previousRecording = profiler::Profiler::IsEnabled();
        m_previousRecordingRequested = profiler::Profiler::IsRecordingRequested();
        editor::ResetProfilerHistory();
        Context().operators = &m_registry;
        Context().undoStack = &m_undo;
        editor::RegisterProfilerOperators(m_registry);
    }

    void TearDown() override
    {
        editor::ResetProfilerHistory();
        profiler::Profiler::RequestCaptureFilter(m_previousFilter);
        profiler::Profiler::SetEnabled(m_previousRecording);
        profiler::Profiler::RequestRecording(m_previousRecordingRequested);
        EditorFixture::TearDown();
    }

    static profiler::PerformanceSnapshot Frame(uint64_t serial, double elapsed = 1.0, uint64_t session = 7)
    {
        profiler::PerformanceSnapshot snapshot;
        snapshot.available = true; snapshot.recording = true; snapshot.complete = true;
        snapshot.captureSessionId = session; snapshot.applicationFrameSerial = serial;
        snapshot.cpuFrameElapsedMs = elapsed; snapshot.wallFrameIntervalAvailable = true;
        snapshot.wallFrameIntervalMs = elapsed * 2;
        return snapshot;
    }

    static profiler::PerformanceSnapshot RepeatedCpuFrame(uint64_t serial, size_t childCalls)
    {
        auto snapshot = Frame(serial, 25.0);
        snapshot.scopeRootSumMs = 20.0;
        const uint64_t rootKey = serial * 2 + 1;
        profiler::PerformanceDescriptor rootDescriptor;
        rootDescriptor.sampleKey = rootKey; rootDescriptor.name = "Root"; rootDescriptor.category = "Animation";
        profiler::PerformanceDescriptor childDescriptor;
        childDescriptor.sampleKey = rootKey + 1; childDescriptor.name = "AnimationPass"; childDescriptor.category = "Animation";
        snapshot.descriptors.push_back(std::move(rootDescriptor)); snapshot.descriptors.push_back(std::move(childDescriptor));
        snapshot.samples.reserve(childCalls + 1);
        profiler::ProfileSample root;
        root.sampleId = 1; root.sampleKey = rootKey; root.inclusiveMs = 20.0; root.selfMs = childCalls == 0 ? 20.0 : 15.0;
        snapshot.samples.push_back(root);
        for (size_t index = 0; index < childCalls; ++index) {
            profiler::ProfileSample child;
            child.sampleId = index + 2; child.parentSampleId = 1; child.sampleKey = rootKey + 1; child.depth = 1;
            child.inclusiveMs = 5.0 / static_cast<double>(childCalls); child.selfMs = child.inclusiveMs;
            snapshot.samples.push_back(child);
        }
        snapshot.recordedSampleCount = static_cast<uint32_t>(snapshot.samples.size());
        return snapshot;
    }

    static renderer::RenderDebugOverlay::Snapshot Rendering(uint64_t serial)
    {
        renderer::RenderDebugOverlay::Snapshot rendering;
        rendering.gpuCurrentView.applicationFrameSerial = serial;
        rendering.renderStats.drawCalls = 23; rendering.renderStats.triangleCount = 450;
        return rendering;
    }

    static void AddScope(profiler::PerformanceSnapshot& snapshot, uint64_t id, uint64_t parent,
        const char* name, double elapsed,
        profiler::ProfileSampleStatus status = profiler::ProfileSampleStatus::COMPLETE,
        profiler::ProfileSampleKind kind = profiler::ProfileSampleKind::TIMED_SCOPE)
    {
        profiler::PerformanceDescriptor descriptor;
        descriptor.sampleKey = id; descriptor.name = name;
        snapshot.descriptors.push_back(std::move(descriptor));
        profiler::ProfileSample sample;
        sample.sampleId = id; sample.parentSampleId = parent; sample.sampleKey = id;
        sample.inclusiveMs = elapsed; sample.status = status; sample.sampleKind = kind;
        snapshot.samples.push_back(sample);
    }
};

TEST_F(ProfilerBenchmarkTest, SkipsBaselineAndWarmupThenRetainsExactlyTheRequestedDistinctFrames)
{
    static_assert(std::is_trivially_copyable_v<editor::PerformanceBenchmarkRow>);
    const auto baseline = Frame(10);
    ASSERT_TRUE(m_capture.Start("movement", 2, 3, baseline));
    const auto* storage = m_capture.Rows().data();
    m_capture.Feed(baseline, Rendering(10));
    EXPECT_EQ(m_capture.Status().warmedFrames, 0u);
    m_capture.Feed(Frame(11), Rendering(11));
    m_capture.Feed(Frame(11), Rendering(11));
    EXPECT_EQ(m_capture.Status().warmedFrames, 1u);
    m_capture.Feed(Frame(12), Rendering(12));
    EXPECT_EQ(m_capture.Status().state, editor::PerformanceBenchmarkState::SAMPLING);
    EXPECT_TRUE(m_capture.Rows().empty());
    for (uint64_t serial = 13; serial <= 16; ++serial) m_capture.Feed(Frame(serial), Rendering(serial));
    ASSERT_EQ(m_capture.Rows().size(), 3u);
    EXPECT_EQ(m_capture.Rows().data(), storage);
    EXPECT_EQ(m_capture.Rows().front().applicationFrameSerial, 13u);
    EXPECT_EQ(m_capture.Rows().back().applicationFrameSerial, 15u);
    EXPECT_EQ(m_capture.Status().state, editor::PerformanceBenchmarkState::COMPLETE);
    EXPECT_EQ(m_capture.Status().captureSessionId, 7u);
    EXPECT_EQ(m_capture.Status().missingFrameCount, 0u);
    EXPECT_EQ(m_capture.CpuSummary().count, 3u);
    EXPECT_TRUE(m_capture.Rows().back().renderingAvailable);
    EXPECT_EQ(m_capture.Rows().back().drawCalls, 23);
}

TEST_F(ProfilerBenchmarkTest, RetainsLongWindowsIndependentlyOfUiHistoryAndUsesNearestRankPercentiles)
{
    ASSERT_TRUE(m_capture.Start("long", 0, 1800, {}));
    const auto* storage = m_capture.Rows().data();
    for (uint64_t serial = 1; serial <= 1800; ++serial) m_capture.Feed(Frame(serial, static_cast<double>(serial)), Rendering(serial));
    EXPECT_EQ(m_capture.Status().state, editor::PerformanceBenchmarkState::COMPLETE);
    EXPECT_EQ(m_capture.Rows().size(), 1800u);
    EXPECT_EQ(m_capture.Rows().data(), storage);
    EXPECT_DOUBLE_EQ(m_capture.CpuSummary().p50Ms, 900);
    EXPECT_DOUBLE_EQ(m_capture.CpuSummary().p95Ms, 1710);
    EXPECT_DOUBLE_EQ(m_capture.CpuSummary().p99Ms, 1782);
    EXPECT_DOUBLE_EQ(m_capture.CpuSummary().maxMs, 1800);
    EXPECT_TRUE(editor::GetPerformanceHistory().frames.empty());
}

TEST_F(ProfilerBenchmarkTest, PreservesIncompleteAndUnavailableMetadataWithoutFabricatingTimingOrRendering)
{
    ASSERT_TRUE(m_capture.Start("metadata", 0, 100, {}));
    for (uint64_t index = 0; index < 100; ++index) {
        auto snapshot = Frame(index + 1, static_cast<double>(100 - index));
        if (index == 0) snapshot.wallFrameIntervalAvailable = false;
        if (index == 1) snapshot.wallFrameIntervalMs = (std::numeric_limits<double>::quiet_NaN)();
        if (index == 5) { snapshot.complete = false; snapshot.droppedSampleCount = 7; }
        m_capture.Feed(snapshot, Rendering(index));
    }
    EXPECT_EQ(m_capture.Status().incompleteFrames, 1u);
    EXPECT_EQ(m_capture.Status().droppedSampleCount, 7u);
    EXPECT_EQ(m_capture.CpuSummary().count, 100u);
    EXPECT_DOUBLE_EQ(m_capture.CpuSummary().p50Ms, 50);
    EXPECT_DOUBLE_EQ(m_capture.CpuSummary().p95Ms, 95);
    EXPECT_DOUBLE_EQ(m_capture.CpuSummary().p99Ms, 99);
    EXPECT_DOUBLE_EQ(m_capture.CpuSummary().maxMs, 100);
    EXPECT_EQ(m_capture.WallSummary().count, 98u);
    EXPECT_DOUBLE_EQ(m_capture.WallSummary().p50Ms, 98);
    EXPECT_DOUBLE_EQ(m_capture.WallSummary().p95Ms, 188);
    EXPECT_DOUBLE_EQ(m_capture.WallSummary().p99Ms, 196);
    EXPECT_FALSE(m_capture.Rows().front().wallFrameIntervalAvailable);
    EXPECT_FALSE(m_capture.Rows()[1].wallFrameIntervalAvailable);
    EXPECT_EQ(m_capture.Rows()[1].wallFrameIntervalMs, 0);
    EXPECT_FALSE(m_capture.Rows().front().renderingAvailable);
    EXPECT_EQ(m_capture.Rows().front().drawCalls, 0);
    EXPECT_FALSE(m_capture.Rows()[5].complete);
}

TEST_F(ProfilerBenchmarkTest, PhysicsCountsWorldStepsAndOnlyOutermostSystemScopesAcrossNestedNames)
{
    ASSERT_TRUE(m_capture.Start("physics_nested", 0, 1, {}));
    const auto* storage = m_capture.Rows().data();
    auto frame = Frame(1, 20);
    AddScope(frame, 1, 0, "PhysicsSystem", 10);
    AddScope(frame, 2, 1, "Unrelated", 9);
    AddScope(frame, 3, 2, "PhysicsSystem", 8);
    AddScope(frame, 4, 3, "PhysicsSystem::SyncRigidBodies", 0.5);
    AddScope(frame, 5, 3, "PhysicsSystem::SyncColliders", 0.8);
    AddScope(frame, 6, 3, "PhysicsSystem::SubmitSceneState", 0.2);
    AddScope(frame, 7, 3, "physics::World::Step", 3);
    AddScope(frame, 8, 3, "PhysicsSystem::DispatchCollisionEvents", 0.4);
    AddScope(frame, 9, 3, "PhysicsSystem::WriteBackTransforms", 0.6);
    AddScope(frame, 10, 0, "PhysicsSystem", 7);
    AddScope(frame, 11, 10, "PhysicsSystem", 6);
    AddScope(frame, 12, 11, "physics::World::Step", 2);
    m_capture.Feed(frame, Rendering(1));
    ASSERT_EQ(m_capture.Rows().size(), 1u);
    EXPECT_EQ(m_capture.Rows().data(), storage);
    const auto& physics = m_capture.Rows().front().physics;
    EXPECT_TRUE(physics.complete);
    EXPECT_EQ(physics.stepCount, 2u);
    const auto& system = physics.scopes[static_cast<size_t>(editor::PhysicsBenchmarkScope::SYSTEM)];
    EXPECT_EQ(system.calls, 2u);
    EXPECT_DOUBLE_EQ(system.totalMs, 17);
    const auto& world = physics.scopes[static_cast<size_t>(editor::PhysicsBenchmarkScope::WORLD_STEP)];
    EXPECT_EQ(world.calls, 2u); EXPECT_DOUBLE_EQ(world.totalMs, 5);
    EXPECT_DOUBLE_EQ(physics.scopes[static_cast<size_t>(editor::PhysicsBenchmarkScope::SYNC_RIGID_BODIES)].totalMs, 0.5);
    EXPECT_DOUBLE_EQ(physics.scopes[static_cast<size_t>(editor::PhysicsBenchmarkScope::SYNC_COLLIDERS)].totalMs, 0.8);
    EXPECT_DOUBLE_EQ(physics.scopes[static_cast<size_t>(editor::PhysicsBenchmarkScope::SUBMIT_SCENE_STATE)].totalMs, 0.2);
    EXPECT_DOUBLE_EQ(physics.scopes[static_cast<size_t>(editor::PhysicsBenchmarkScope::DISPATCH_COLLISION_EVENTS)].totalMs, 0.4);
    EXPECT_DOUBLE_EQ(physics.scopes[static_cast<size_t>(editor::PhysicsBenchmarkScope::WRITE_BACK_TRANSFORMS)].totalMs, 0.6);
    const auto& summary = m_capture.PhysicsSummary();
    EXPECT_EQ(summary.stepCount, 2u); EXPECT_EQ(summary.multiStepFrames, 1u);
    EXPECT_DOUBLE_EQ(summary.scopes[static_cast<size_t>(editor::PhysicsBenchmarkScope::WORLD_STEP)].msPerStep.p50Ms, 2.5);
}

TEST_F(ProfilerBenchmarkTest, PhysicsIncludesZeroStepFramesButNormalizesOnlyFramesWithCompletedSteps)
{
    ASSERT_TRUE(m_capture.Start("physics_steps", 0, 3, {}));
    m_capture.Feed(Frame(1), Rendering(1));
    auto single = Frame(2, 5);
    AddScope(single, 1, 0, "physics::World::Step", 4);
    m_capture.Feed(single, Rendering(2));
    auto multiple = Frame(3, 7);
    AddScope(multiple, 1, 0, "physics::World::Step", 1);
    AddScope(multiple, 2, 0, "physics::World::Step", 5);
    m_capture.Feed(multiple, Rendering(3));
    const auto& summary = m_capture.PhysicsSummary();
    EXPECT_EQ(summary.validFrames, 3u); EXPECT_EQ(summary.excludedFrames, 0u);
    EXPECT_EQ(summary.stepCount, 3u); EXPECT_EQ(summary.observedStepCount, 3u);
    EXPECT_EQ(summary.zeroStepFrames, 1u); EXPECT_EQ(summary.multiStepFrames, 1u);
    const auto& world = summary.scopes[static_cast<size_t>(editor::PhysicsBenchmarkScope::WORLD_STEP)];
    EXPECT_EQ(world.calls, 3u); EXPECT_DOUBLE_EQ(world.totalMs, 10);
    EXPECT_EQ(world.frameMs.count, 3u); EXPECT_DOUBLE_EQ(world.frameMs.p50Ms, 4);
    EXPECT_DOUBLE_EQ(world.frameMs.p95Ms, 6);
    EXPECT_EQ(world.msPerStep.count, 2u); EXPECT_DOUBLE_EQ(world.msPerStep.p50Ms, 3);
    EXPECT_DOUBLE_EQ(world.msPerStep.p95Ms, 4);
    EXPECT_EQ(m_capture.Rows().front().physics.stepCount, 0u);
    ASSERT_TRUE(m_capture.Start("physics_reset", 0, 1, {}));
    m_capture.Feed(Frame(4), Rendering(4));
    EXPECT_EQ(m_capture.PhysicsSummary().stepCount, 0u);
    EXPECT_EQ(m_capture.PhysicsSummary().zeroStepFrames, 1u);
    EXPECT_EQ(m_capture.PhysicsSummary().scopes[static_cast<size_t>(editor::PhysicsBenchmarkScope::WORLD_STEP)].msPerStep.count, 0u);
}

TEST_F(ProfilerBenchmarkTest, PhysicsRetainsFaultCountersAndExcludesPartialOrInvalidFramesFromDistributions)
{
    ASSERT_TRUE(m_capture.Start("physics_partial", 0, 8, {}));
    auto valid = Frame(1);
    AddScope(valid, 1, 0, "physics::World::Step", 0.5);
    m_capture.Feed(valid, Rendering(1));
    auto incomplete = Frame(2); incomplete.complete = false;
    AddScope(incomplete, 1, 0, "physics::World::Step", 0.4);
    m_capture.Feed(incomplete, Rendering(2));
    auto dropped = Frame(3); dropped.droppedSampleCount = 1;
    AddScope(dropped, 1, 0, "physics::World::Step", 0.3);
    m_capture.Feed(dropped, Rendering(3));
    auto faulted = Frame(4);
    AddScope(faulted, 1, 0, "physics::World::Step", 0.2, profiler::ProfileSampleStatus::FAULTED);
    m_capture.Feed(faulted, Rendering(4));
    auto aborted = Frame(5);
    AddScope(aborted, 1, 0, "physics::World::Step", 0.1, profiler::ProfileSampleStatus::ABORTED);
    m_capture.Feed(aborted, Rendering(5));
    auto invalid = Frame(6);
    AddScope(invalid, 1, 0, "physics::World::Step", (std::numeric_limits<double>::quiet_NaN)());
    m_capture.Feed(invalid, Rendering(6));
    auto external = Frame(7);
    AddScope(external, 1, 0, "physics::World::Step", 1, profiler::ProfileSampleStatus::COMPLETE,
        profiler::ProfileSampleKind::EXTERNAL_DURATION);
    m_capture.Feed(external, Rendering(7));
    auto missingParent = Frame(8);
    AddScope(missingParent, 1, 17, "PhysicsSystem", 1);
    m_capture.Feed(missingParent, Rendering(8));
    const auto& summary = m_capture.PhysicsSummary();
    EXPECT_EQ(summary.validFrames, 1u); EXPECT_EQ(summary.excludedFrames, 7u);
    EXPECT_EQ(summary.observedStepCount, 3u); EXPECT_EQ(summary.stepCount, 1u);
    EXPECT_EQ(summary.faultedScopeCount, 1u); EXPECT_EQ(summary.abortedScopeCount, 1u);
    EXPECT_EQ(summary.invalidScopeCount, 3u);
    const auto& world = summary.scopes[static_cast<size_t>(editor::PhysicsBenchmarkScope::WORLD_STEP)];
    EXPECT_EQ(world.frameMs.count, 1u); EXPECT_DOUBLE_EQ(world.frameMs.maxMs, 0.5);
    EXPECT_EQ(m_capture.CpuSummary().count, 8u);
    EXPECT_EQ(m_capture.Rows()[1].physics.stepCount, 1u);
    EXPECT_FALSE(m_capture.Rows()[1].physics.complete);
    EXPECT_FALSE(m_capture.Rows()[2].physics.complete);
    EXPECT_EQ(m_capture.Rows()[3].physics.faultedScopeCount, 1u);
    EXPECT_EQ(m_capture.Rows()[3].physics.stepCount, 0u);
}

TEST_F(ProfilerBenchmarkTest, RejectsOverlapsAndNeverMixesCaptureSessionsOrRewoundFrames)
{
    EXPECT_FALSE(m_capture.Start("", 0, 1, {}));
    EXPECT_FALSE(m_capture.Start("invalid", 0, 0, {}));
    ASSERT_TRUE(m_capture.Start("session", 0, 4, {}));
    EXPECT_FALSE(m_capture.Start("overlap", 0, 2, {}));
    m_capture.Feed(Frame(1), Rendering(1));
    m_capture.Feed(Frame(3), Rendering(3));
    EXPECT_EQ(m_capture.Status().missingFrameCount, 1u);
    m_capture.Feed(Frame(4, 1, 8), Rendering(4));
    EXPECT_EQ(m_capture.Status().state, editor::PerformanceBenchmarkState::FAILED);
    EXPECT_EQ(m_capture.Status().failure, editor::PerformanceBenchmarkFailure::SESSION_CHANGED);
    EXPECT_EQ(m_capture.Rows().size(), 2u);
    ASSERT_TRUE(m_capture.Start("rewind", 0, 2, {}));
    m_capture.Feed(Frame(5), Rendering(5));
    m_capture.Feed(Frame(4), Rendering(4));
    EXPECT_EQ(m_capture.Status().failure, editor::PerformanceBenchmarkFailure::FRAME_ORDER_CHANGED);
    ASSERT_TRUE(m_capture.Start("stopped", 0, 2, {}));
    m_capture.Feed(Frame(6), Rendering(6));
    auto stopped = Frame(7); stopped.recording = false;
    m_capture.Feed(stopped, Rendering(7));
    EXPECT_EQ(m_capture.Status().failure, editor::PerformanceBenchmarkFailure::RECORDING_STOPPED);
    ASSERT_TRUE(m_capture.Start("cancel", 0, 2, {}));
    EXPECT_TRUE(m_capture.Cancel());
    EXPECT_FALSE(m_capture.Cancel());
    EXPECT_EQ(m_capture.Status().state, editor::PerformanceBenchmarkState::CANCELLED);
}

TEST_F(ProfilerBenchmarkTest, WaitsForDisabledBaselineThenSamplesExactlyOneHundredTwentyFramesAfterWarmup)
{
    auto baseline = Frame(10); baseline.recording = false; baseline.available = false;
    ASSERT_TRUE(m_capture.Start("disabled", 2, 120, baseline));
    m_capture.Feed(baseline, Rendering(10));
    EXPECT_TRUE(m_capture.IsActive());
    EXPECT_EQ(m_capture.Status().warmedFrames, 0u);
    auto opening = Frame(0, 0, 8); opening.available = false;
    m_capture.Feed(opening, Rendering(0));
    EXPECT_EQ(m_capture.Status().warmedFrames, 0u);
    for (uint64_t serial = 11; serial <= 133; ++serial) {
        m_capture.Feed(Frame(serial, 1, 8), Rendering(serial));
        m_capture.Feed(Frame(serial, 1, 8), Rendering(serial));
    }
    EXPECT_EQ(m_capture.Status().state, editor::PerformanceBenchmarkState::COMPLETE);
    EXPECT_EQ(m_capture.Status().warmedFrames, 2u);
    EXPECT_EQ(m_capture.Status().capturedFrames, 120u);
    ASSERT_EQ(m_capture.Rows().size(), 120u);
    EXPECT_EQ(m_capture.Rows().front().applicationFrameSerial, 13u);
    EXPECT_EQ(m_capture.Rows().back().applicationFrameSerial, 132u);
    EXPECT_EQ(m_capture.Status().captureSessionId, 8u);
}

TEST_F(ProfilerBenchmarkTest, StoppingBeforeTheFirstAcceptedFrameNeverLeavesAWaitingCaptureActive)
{
    const auto baseline = Frame(10);
    ASSERT_TRUE(m_capture.Start("stopped_frame", 0, 120, baseline));
    auto stopped = Frame(11); stopped.recording = false;
    m_capture.Feed(stopped, Rendering(11));
    EXPECT_EQ(m_capture.Status().failure, editor::PerformanceBenchmarkFailure::RECORDING_STOPPED);
    EXPECT_FALSE(m_capture.IsActive());
    ASSERT_TRUE(m_capture.Start("stopped_session", 0, 120, baseline));
    stopped = Frame(0, 0, 8); stopped.recording = false; stopped.available = false;
    m_capture.Feed(stopped, Rendering(0));
    EXPECT_EQ(m_capture.Status().failure, editor::PerformanceBenchmarkFailure::RECORDING_STOPPED);
    EXPECT_FALSE(m_capture.IsActive());
    ASSERT_TRUE(m_capture.Start("cancelled_pending_enable", 0, 120, stopped));
    m_capture.Feed(stopped, Rendering(0), false);
    EXPECT_EQ(m_capture.Status().failure, editor::PerformanceBenchmarkFailure::RECORDING_STOPPED);
    EXPECT_EQ(m_capture.Status().capturedFrames, 0u);
    EXPECT_FALSE(m_capture.IsActive());
}

TEST_F(ProfilerBenchmarkTest, ConsecutiveCapturesOverridePendingStopsAndRestoreThePriorRequestedState)
{
    profiler::Profiler::SetEnabled(true);
    editor::OpArgs recording; recording.Set("recording", false);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "profiler.performance.set_recording", recording).ok);
    EXPECT_TRUE(profiler::Profiler::IsEnabled());
    EXPECT_FALSE(profiler::Profiler::IsRecordingRequested());
    editor::OpArgs start;
    start.Set("label", std::string("cancelled_a")); start.Set("sampleFrames", 1);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "profiler.benchmark.start", start).ok);
    EXPECT_TRUE(profiler::Profiler::IsRecordingRequested());
    ASSERT_TRUE(editor::CancelPerformanceBenchmark());
    EXPECT_FALSE(profiler::Profiler::IsRecordingRequested());
    EXPECT_TRUE(profiler::Profiler::IsEnabled());
    ASSERT_TRUE(editor::StartPerformanceBenchmark("completed_b", 0, 1));
    EXPECT_TRUE(profiler::Profiler::IsRecordingRequested());
    const auto& baseline = profiler::Profiler::GetSnapshot();
    const auto serial = baseline.applicationFrameSerial + 1;
    editor::GetPerformanceBenchmarkCapture().Feed(Frame(serial, 1, baseline.captureSessionId), Rendering(serial));
    EXPECT_EQ(editor::GetPerformanceBenchmarkCapture().Status().state, editor::PerformanceBenchmarkState::COMPLETE);
    editor::TickProfilerHistory(Context());
    EXPECT_FALSE(profiler::Profiler::IsRecordingRequested());
    EXPECT_TRUE(profiler::Profiler::IsEnabled());
    ASSERT_TRUE(editor::StartPerformanceBenchmark("started_c", 0, 1));
    EXPECT_TRUE(profiler::Profiler::IsRecordingRequested());
    ASSERT_TRUE(editor::CancelPerformanceBenchmark());
    EXPECT_FALSE(profiler::Profiler::IsRecordingRequested());

    profiler::Profiler::SetEnabled(false);
    profiler::Profiler::RequestRecording(true);
    ASSERT_TRUE(editor::StartPerformanceBenchmark("preserved_pending_enable", 0, 1));
    ASSERT_TRUE(editor::CancelPerformanceBenchmark());
    EXPECT_TRUE(profiler::Profiler::IsRecordingRequested());
    EXPECT_FALSE(profiler::Profiler::IsEnabled());
}

TEST_F(ProfilerBenchmarkTest, OperatorsExposeCheapStatusAndPaginatedRowsOnlyAfterCompletion)
{
    EXPECT_EQ(m_registry.Find("profiler.benchmark.start")->kind, editor::OpKind::Action);
    EXPECT_EQ(m_registry.Find("profiler.benchmark.status")->kind, editor::OpKind::Query);
    EXPECT_EQ(m_registry.Find("profiler.benchmark.result")->kind, editor::OpKind::Query);
    editor::OpArgs start;
    start.Set("label", std::string("operators")); start.Set("warmupFrames", -1); start.Set("sampleFrames", 3);
    EXPECT_EQ(editor::InvokeOperator(Context(), "profiler.benchmark.start", start).errorCode, "BAD_ARG");
    start.Set("warmupFrames", 0);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "profiler.benchmark.start", start).ok);
    EXPECT_EQ(editor::InvokeOperator(Context(), "profiler.benchmark.start", start).errorCode, "BENCHMARK_ACTIVE");
    EXPECT_EQ(editor::InvokeOperator(Context(), "profiler.benchmark.result").errorCode, "NOT_COMPLETE");
    auto status = editor::InvokeOperator(Context(), "profiler.benchmark.status");
    ASSERT_TRUE(status.ok);
    EXPECT_EQ(status.data.Find("rows"), nullptr);
    auto& capture = editor::GetPerformanceBenchmarkCapture();
    uint64_t serial = profiler::Profiler::GetSnapshot().applicationFrameSerial + 1;
    const uint64_t session = profiler::Profiler::GetSnapshot().captureSessionId;
    for (uint64_t index = 0; index < 3; ++index) {
        auto frame = Frame(serial + index, static_cast<double>(index + 1), session);
        if (index == 1) {
            AddScope(frame, 1, 0, "physics::World::Step", 0.25);
            AddScope(frame, 2, 0, "physics::World::Step", 0.75);
        }
        capture.Feed(frame, Rendering(serial + index));
    }
    editor::OpArgs page; page.Set("offset", 1); page.Set("count", 1);
    const auto result = editor::InvokeOperator(Context(), "profiler.benchmark.result", page);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.data.Find("count")->AsInt(), 1);
    EXPECT_EQ(result.data.Find("nextOffset")->AsInt(), 2);
    EXPECT_TRUE(result.data.Find("hasMore")->AsBool());
    EXPECT_EQ(result.data.Find("rows")->AsArray().size(), 1u);
    EXPECT_DOUBLE_EQ(result.data.Find("cpu")->Find("p50Ms")->AsDouble(), 2);
    const auto* physics = result.data.Find("physics");
    ASSERT_NE(physics, nullptr);
    EXPECT_EQ(physics->Find("stepCount")->AsString(), "2");
    EXPECT_EQ(physics->Find("zeroStepFrames")->AsString(), "2");
    EXPECT_EQ(physics->Find("multiStepFrames")->AsString(), "1");
    const auto* world = physics->Find("scopes")->Find("physics::World::Step");
    ASSERT_NE(world, nullptr);
    EXPECT_TRUE(world->Find("meanMsPerStepAvailable")->AsBool());
    EXPECT_DOUBLE_EQ(world->Find("meanMsPerStep")->AsDouble(), 0.5);
    EXPECT_EQ(world->Find("frameMs")->Find("count")->AsInt(), 3);
    EXPECT_DOUBLE_EQ(world->Find("frameMs")->Find("p50Ms")->AsDouble(), 0);
    const auto* physicsRow = result.data.Find("rows")->AsArray().front().Find("physics");
    ASSERT_NE(physicsRow, nullptr);
    EXPECT_EQ(physicsRow->Find("stepCount")->AsInt(), 2);
    EXPECT_DOUBLE_EQ(physicsRow->Find("scopes")->Find("physics::World::Step")->Find("totalMs")->AsDouble(), 1);
    page.Set("offset", 3);
    EXPECT_FALSE(editor::InvokeOperator(Context(), "profiler.benchmark.result", page).data.Find("hasMore")->AsBool());
    page.Set("offset", 4);
    EXPECT_EQ(editor::InvokeOperator(Context(), "profiler.benchmark.result", page).errorCode, "BAD_ARG");
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(ProfilerBenchmarkTest, ComparesOnlyCompletedCapturesWithMatchingDeclaredAndObservedConditions)
{
    const editor::PerformanceBenchmarkConditions conditions{"sea-1km/replay-v1", "fixed-camera-v1", "dt=1/60 seed=42 quality=high pacing=off build=Development"};
    ASSERT_TRUE(m_capture.Start("before", 1, 2, {}, conditions));
    for (uint64_t i = 1; i <= 3; ++i) m_capture.Feed(Frame(i, static_cast<double>(i)), Rendering(i));
    const auto before = m_capture.Report();
    ASSERT_TRUE(m_capture.Start("after", 1, 2, {}, conditions));
    for (uint64_t i = 1; i <= 3; ++i) m_capture.Feed(Frame(i, static_cast<double>(i) - 1), Rendering(i));
    const auto after = m_capture.Report();
    const auto comparison = editor::ComparePerformanceBenchmarks(before, after);
    EXPECT_TRUE(comparison.comparable);
    EXPECT_TRUE(comparison.warnings.empty());
    EXPECT_DOUBLE_EQ(comparison.cpuP50DeltaMs, -1);
    EXPECT_DOUBLE_EQ(comparison.cpuP95DeltaMs, -1);
    auto changed = after; changed.captureFilter.scope = "Extract";
    EXPECT_FALSE(editor::ComparePerformanceBenchmarks(before, changed).comparable);
    changed = after; changed.conditions.configuration = "seed=43";
    EXPECT_FALSE(editor::ComparePerformanceBenchmarks(before, changed).comparable);
    changed = after; changed.status.sampleFrames = 3;
    EXPECT_FALSE(editor::ComparePerformanceBenchmarks(before, changed).comparable);
    changed = after; changed.status.missingFrameCount = 1;
    EXPECT_FALSE(editor::ComparePerformanceBenchmarks(before, changed).comparable);
    changed = after; changed.status.incompleteFrames = 1; changed.status.droppedSampleCount = 7;
    const auto partial = editor::ComparePerformanceBenchmarks(before, changed);
    EXPECT_FALSE(partial.comparable); EXPECT_FALSE(partial.warnings.empty());
    EXPECT_DOUBLE_EQ(partial.cpuP50DeltaMs, 0);
    changed = after; changed.conditions = {};
    EXPECT_FALSE(editor::ComparePerformanceBenchmarks(before, changed).comparable);
    changed = after; changed.width = 2048;
    EXPECT_FALSE(editor::ComparePerformanceBenchmarks(before, changed).comparable);
}

TEST_F(ProfilerBenchmarkTest, DetectsObservedViewportChangesAndDoesNotFabricateFilteredPhysicsCosts)
{
    profiler::PerformanceSnapshot baseline;
    baseline.captureFilter = {"Rendering", "Extract"};
    ASSERT_TRUE(m_capture.Start("filtered", 0, 2, baseline, {"scene", "camera", "fixed"}));
    auto frame = Frame(1); frame.captureFilter = baseline.captureFilter;
    auto rendering = Rendering(1); rendering.gpuCurrentView.width = 1920;
    m_capture.Feed(frame, rendering);
    frame = Frame(2); frame.captureFilter = baseline.captureFilter;
    rendering = Rendering(2); rendering.gpuCurrentView.width = 1280;
    m_capture.Feed(frame, rendering);
    EXPECT_TRUE(m_capture.Report().renderingConditionsChanged);
    EXPECT_EQ(m_capture.PhysicsSummary().validFrames, 0u);
    EXPECT_EQ(m_capture.PhysicsSummary().excludedFrames, 2u);
    EXPECT_FALSE(editor::ComparePerformanceBenchmarks(m_capture.Report(), m_capture.Report()).comparable);
    EXPECT_FALSE(m_capture.Start("too_large", 0, 60001, {}));
}

TEST_F(ProfilerBenchmarkTest, OperatorsExposeLossReasonsCollectionRangeAndIncompleteComparisonWarnings)
{
    profiler::Profiler::SetEnabled(false);
    profiler::Profiler::SetEnabled(true);
    editor::OpArgs filter; filter.Set("category", std::string("Rendering")); filter.Set("scope", std::string("Extract"));
    ASSERT_TRUE(editor::InvokeOperator(Context(), "profiler.performance.set_capture_filter", filter).ok);
    profiler::Profiler::BeginFrame();
    for (size_t i = 0; i < profiler::ProfileRecorder::MAX_SAMPLES + 5; ++i)
        profiler::Profiler::PushMarker(profiler::ProfilerMarker("Extract", "Rendering"));
    profiler::Profiler::PushMarker(profiler::ProfilerMarker("ignored", "Scripts"));
    profiler::Profiler::EndFrame();
    const auto snapshot = editor::InvokeOperator(Context(), "profiler.performance.snapshot");
    ASSERT_TRUE(snapshot.ok);
    EXPECT_EQ(snapshot.data.Find("sampleCapacity")->AsInt(), 8192);
    EXPECT_EQ(snapshot.data.Find("capacityDroppedSampleCount")->AsInt(), 5);
    EXPECT_EQ(snapshot.data.Find("filteredSampleCount")->AsInt(), 1);
    EXPECT_FALSE(snapshot.data.Find("warning")->AsString().empty());
    EXPECT_EQ(snapshot.data.Find("captureFilter")->Find("scope")->AsString(), "Extract");
    editor::OpArgs start; start.Set("label", std::string("partial")); start.Set("warmupFrames", 0); start.Set("sampleFrames", 1);
    start.Set("scenario", std::string("scene")); start.Set("camera", std::string("camera")); start.Set("configuration", std::string("dt=1/60 seed=42"));
    ASSERT_TRUE(editor::InvokeOperator(Context(), "profiler.benchmark.start", start).ok);
    EXPECT_EQ(editor::InvokeOperator(Context(), "profiler.performance.set_capture_filter", filter).errorCode, "BENCHMARK_ACTIVE");
    auto frame = profiler::Profiler::GetSnapshot();
    ++frame.applicationFrameSerial;
    editor::GetPerformanceBenchmarkCapture().Feed(frame, Rendering(frame.applicationFrameSerial));
    const auto result = editor::InvokeOperator(Context(), "profiler.benchmark.result");
    ASSERT_TRUE(result.ok);
    EXPECT_FALSE(result.data.Find("comparison")->Find("eligible")->AsBool());
    EXPECT_FALSE(result.data.Find("comparison")->Find("warnings")->AsArray().empty());
    EXPECT_EQ(result.data.Find("conditions")->Find("configuration")->AsString(), "dt=1/60 seed=42");
    EXPECT_TRUE(result.data.Find("summaryIncludesIncompleteFrames")->AsBool());
    ASSERT_TRUE(editor::InvokeOperator(Context(), "profiler.benchmark.set_reference").ok);
    EXPECT_TRUE(editor::GetPerformanceBenchmarkReference() != nullptr);
    std::string json;
    EXPECT_TRUE(editor::WriteOpDataJson(result.data, json));
    EXPECT_NE(json.find("sampleCapacity"), std::string::npos);
}

TEST_F(ProfilerBenchmarkTest, MergesFullCapacityRepeatedScopesWithoutDiscardingACompleteCpuFrame)
{
    const auto snapshot = RepeatedCpuFrame(1, profiler::ProfileRecorder::MAX_SAMPLES - 1);
    const auto tree = editor::detail::BuildRecordedProfTree(snapshot);
    ASSERT_TRUE(tree.complete);
    EXPECT_EQ(tree.unavailableReason, nullptr);
    ASSERT_EQ(tree.nodes.size(), 2u);
    ASSERT_EQ(tree.flat.size(), 2u);
    ASSERT_EQ(tree.roots.size(), 1u);
    EXPECT_EQ(tree.nodes[0].children.size(), 1u);
    EXPECT_EQ(tree.nodes[1].calls, static_cast<int>(profiler::ProfileRecorder::MAX_SAMPLES - 1));
    EXPECT_NEAR(tree.nodes[1].totalMs, 5.0, 1e-9);
    EXPECT_NEAR(tree.flat[1].selfMs, 5.0, 1e-9);
    EXPECT_DOUBLE_EQ(tree.nodes[0].totalMs, 20.0);
}

TEST_F(ProfilerBenchmarkTest, KeepsMergedCpuSeriesAcrossThousandsOfFramesDescriptorTurnoverAndSlowDisplayUpdates)
{
    editor::PerformanceHistory captures;
    editor::detail::AnalysisProfHistory projected;
    std::string key;
    for (uint64_t serial = 1; serial <= 2048; ++serial) {
        auto frame = std::make_shared<editor::PerformanceCapture>();
        static_cast<profiler::PerformanceSnapshot&>(*frame) = RepeatedCpuFrame(serial,
            serial % 17 == 0 ? profiler::ProfileRecorder::MAX_SAMPLES - 1 : 1);
        frame->captureSessionId = serial <= 1024 ? 7 : 8;
        if (serial % 127 == 0) { frame->complete = false; frame->droppedSampleCount = 1; }
        captures.frames.push_back(frame);
        while (captures.frames.size() > 240) captures.frames.pop_front();
        captures.frozen = serial % 257 < 17;
        if (!captures.frozen || !captures.selected) captures.selected = frame;
        if (serial % 23 != 0 && serial != 2048) continue;
        editor::detail::ProjectSelectedHistory(captures, projected.cursor, size_t{60},
            [&](const editor::PerformanceCapture& capture) {
                const auto tree = editor::detail::BuildRecordedProfTree(capture);
                ASSERT_EQ(tree.complete, capture.complete);
                ASSERT_EQ(tree.flat.size(), 2u);
                if (key.empty()) key = tree.flat[1].key;
                EXPECT_EQ(tree.flat[1].key, key);
                editor::detail::PushAnalysisProfHistory(projected, tree, capture.cpuFrameElapsedMs, 60);
            }, [&] { projected = {}; });
        editor::detail::UpdateAnalysisProfStats(projected);
        ASSERT_LE(projected.frames.size(), 60u);
        ASSERT_TRUE(projected.stats.contains(key));
        EXPECT_DOUBLE_EQ(projected.stats.at(key).avg, 5.0);
        EXPECT_DOUBLE_EQ(projected.stats.at(key).peak, 5.0);
        const auto values = editor::detail::AnalysisProfSeriesValues(projected, key);
        ASSERT_EQ(values.size(), projected.frames.size());
        for (size_t index = 0; index < values.size(); ++index) {
            if (projected.completeFrames[index]) EXPECT_FLOAT_EQ(values[index], 5.0f);
            else EXPECT_TRUE(std::isnan(values[index]));
        }
        EXPECT_FALSE(editor::detail::ProjectSelectedHistory(captures, projected.cursor, size_t{60},
            [](const auto&) { ADD_FAILURE() << "Duplicate completed frame projection"; }, [] {}));
    }
    EXPECT_EQ(projected.cursor.session, 8u);
    EXPECT_EQ(projected.cursor.serial, captures.selected->applicationFrameSerial);
}

TEST_F(ProfilerBenchmarkTest, DistinguishesMeasuredZeroAbsentCompleteScopeMissingRangeAndUnfinishedIntervals)
{
    editor::detail::AnalysisProfHistory history;
    auto snapshot = RepeatedCpuFrame(1, 1);
    const auto normal = editor::detail::BuildRecordedProfTree(snapshot);
    ASSERT_EQ(normal.flat.size(), 2u);
    const auto flatKey = normal.flat[1].key;
    const auto totalKey = normal.nodes[1].key;
    editor::detail::PushAnalysisProfHistory(history, normal, 10.0, 60);
    auto zero = RepeatedCpuFrame(2, 1);
    zero.samples[0].selfMs = 20.0; zero.samples[1].inclusiveMs = 0.0; zero.samples[1].selfMs = 0.0;
    const auto zeroTree = editor::detail::BuildRecordedProfTree(zero);
    ASSERT_TRUE(zeroTree.complete);
    EXPECT_TRUE(zeroTree.flat[1].totalAvailable);
    EXPECT_TRUE(zeroTree.flat[1].selfAvailable);
    EXPECT_EQ(zeroTree.flat[1].calls, 1);
    editor::detail::PushAnalysisProfHistory(history, zeroTree, 10.0, 60);
    auto absent = RepeatedCpuFrame(3, 0);
    editor::detail::PushAnalysisProfHistory(history, editor::detail::BuildRecordedProfTree(absent), 10.0, 60);
    auto unfinished = RepeatedCpuFrame(4, 1);
    unfinished.samples[1].status = profiler::ProfileSampleStatus::ABORTED;
    const auto unfinishedTree = editor::detail::BuildRecordedProfTree(unfinished);
    EXPECT_FALSE(unfinishedTree.complete);
    EXPECT_FALSE(unfinishedTree.flat[1].totalAvailable);
    editor::detail::PushAnalysisProfHistory(history, unfinishedTree, 10.0, 60);
    auto filteredSelf = RepeatedCpuFrame(5, 1);
    filteredSelf.samples[1].selfAvailable = false;
    editor::detail::PushAnalysisProfHistory(history, editor::detail::BuildRecordedProfTree(filteredSelf), 10.0, 60);
    editor::detail::UpdateAnalysisProfStats(history);
    const auto selfValues = editor::detail::AnalysisProfSeriesValues(history, flatKey);
    const auto totalValues = editor::detail::AnalysisProfSeriesValues(history, totalKey);
    ASSERT_EQ(selfValues.size(), 5u);
    EXPECT_FLOAT_EQ(selfValues[0], 5.0f);
    EXPECT_FLOAT_EQ(selfValues[1], 0.0f);
    EXPECT_FLOAT_EQ(selfValues[2], 0.0f);
    EXPECT_TRUE(std::isnan(selfValues[3]));
    EXPECT_TRUE(std::isnan(selfValues[4]));
    EXPECT_TRUE(std::isnan(totalValues[3]));
    EXPECT_FLOAT_EQ(totalValues[4], 5.0f);
    for (float value : editor::detail::AnalysisProfSeriesValues(history, "outside captured range"))
        EXPECT_TRUE(std::isnan(value));
}

TEST_F(ProfilerBenchmarkTest, PreservesPartialScopeRowsAndReliableFrameScalarWhenUniqueRowsReachTheBudget)
{
    auto snapshot = Frame(1, 10.0);
    snapshot.scopeRootSumMs = 8.192;
    snapshot.descriptors.reserve(profiler::ProfileRecorder::MAX_SAMPLES);
    snapshot.samples.reserve(profiler::ProfileRecorder::MAX_SAMPLES);
    for (uint64_t id = 1; id <= profiler::ProfileRecorder::MAX_SAMPLES; ++id) {
        profiler::PerformanceDescriptor descriptor;
        descriptor.sampleKey = id; descriptor.name = "Unique_" + std::to_string(id); descriptor.category = "Animation";
        snapshot.descriptors.push_back(std::move(descriptor));
        profiler::ProfileSample sample;
        sample.sampleId = id; sample.sampleKey = id; sample.inclusiveMs = 0.001; sample.selfMs = 0.001;
        snapshot.samples.push_back(sample);
    }
    const auto tree = editor::detail::BuildRecordedProfTree(snapshot);
    ASSERT_FALSE(tree.complete);
    ASSERT_NE(tree.unavailableReason, nullptr);
    ASSERT_FALSE(tree.nodes.empty());
    EXPECT_LT(tree.nodes.size(), snapshot.samples.size());
    EXPECT_TRUE(tree.frameTotalAvailable);
    editor::detail::AnalysisProfHistory history;
    editor::detail::PushAnalysisProfHistory(history, tree, 10.0, 60);
    EXPECT_TRUE(std::isnan(editor::detail::AnalysisProfSeriesValues(history, tree.nodes.front().key).front()));
    EXPECT_FLOAT_EQ(editor::detail::AnalysisProfSeriesValues(history, {}).front(), 8.192f);
    EXPECT_FLOAT_EQ(history.elapsedMs.front(), 10.0f);
}

TEST_F(ProfilerBenchmarkTest, UpdatesOwnedCpuHistoryThroughoutBenchmarkWarmupAndSamplingWithoutDuplicateFrames)
{
    profiler::Profiler::SetEnabled(false); profiler::Profiler::SetEnabled(true);
    profiler::Profiler::BeginFrame(9000, profiler::ProfileRecorder::NowMs());
    profiler::Profiler::PushSample(profiler::ProfilerMarker("AnimationPass", "Animation"), 5.0);
    profiler::Profiler::EndFrame();
    editor::TickProfilerHistory(Context());
    editor::OpArgs args;
    args.Set("label", std::string("live cpu")); args.Set("warmupFrames", 2); args.Set("sampleFrames", 64);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "profiler.benchmark.start", args).ok);
    for (uint64_t serial = 9001; serial <= 9066; ++serial) {
        profiler::Profiler::BeginFrame(serial, profiler::ProfileRecorder::NowMs());
        profiler::Profiler::PushSample(profiler::ProfilerMarker("AnimationPass", "Animation"), 5.0);
        profiler::Profiler::EndFrame();
        editor::TickProfilerHistory(Context());
        auto& history = editor::GetPerformanceHistory();
        ASSERT_NE(history.selected, nullptr);
        EXPECT_EQ(history.selected->applicationFrameSerial, serial);
        EXPECT_EQ(history.frames.size(), serial - 8999);
        const size_t count = history.frames.size();
        editor::TickProfilerHistory(Context());
        EXPECT_EQ(history.frames.size(), count);
        EXPECT_EQ(history.selected->recordedSampleCount, 1u);
    }
    EXPECT_EQ(editor::GetPerformanceBenchmarkCapture().Status().state, editor::PerformanceBenchmarkState::COMPLETE);
    EXPECT_EQ(editor::GetPerformanceBenchmarkCapture().Status().capturedFrames, 64u);
}

TEST_F(ProfilerBenchmarkTest, KeepsTemporaryRecordingEnabledThroughSamplingThenRestoresTheStoppedState)
{
    profiler::Profiler::SetEnabled(false);
    editor::OpArgs args;
    args.Set("label", std::string("temporary live cpu")); args.Set("warmupFrames", 2); args.Set("sampleFrames", 8);
    ASSERT_TRUE(editor::InvokeOperator(Context(), "profiler.benchmark.start", args).ok);
    for (uint64_t serial = 10000; serial < 10010; ++serial) {
        profiler::Profiler::BeginFrame(serial, profiler::ProfileRecorder::NowMs());
        profiler::Profiler::PushSample(profiler::ProfilerMarker("AnimationPass", "Animation"), 5.0);
        profiler::Profiler::EndFrame();
        editor::TickProfilerHistory(Context());
        ASSERT_TRUE(profiler::Profiler::GetSnapshot().recording);
        ASSERT_NE(editor::GetPerformanceHistory().selected, nullptr);
        EXPECT_EQ(editor::GetPerformanceHistory().selected->applicationFrameSerial, serial);
        if (serial < 10009) EXPECT_TRUE(profiler::Profiler::IsRecordingRequested());
    }
    EXPECT_EQ(editor::GetPerformanceBenchmarkCapture().Status().state, editor::PerformanceBenchmarkState::COMPLETE);
    EXPECT_EQ(editor::GetPerformanceBenchmarkCapture().Status().capturedFrames, 8u);
    EXPECT_FALSE(profiler::Profiler::IsRecordingRequested());
    profiler::Profiler::BeginFrame(10010, profiler::ProfileRecorder::NowMs());
    profiler::Profiler::EndFrame();
    editor::TickProfilerHistory(Context());
    EXPECT_FALSE(profiler::Profiler::IsEnabled());
    EXPECT_FALSE(profiler::Profiler::GetSnapshot().available);
    EXPECT_EQ(editor::GetPerformanceHistory().selected->applicationFrameSerial, 10009u);
}


TEST_F(ProfilerBenchmarkTest, DistinguishesSameNameNestedInclusiveFromElapsedAndPreservesCanonicalOuterScopeAfterRename)
{
    auto legacy = Frame(1, 150.0);
    legacy.scopeRootSumMs = 150.0;
    AddScope(legacy, 1, 0, "LateScriptSystem", 150.0);
    AddScope(legacy, 2, 1, "LateScriptSystem", 149.0);
    legacy.samples[0].selfMs = 1.0;
    legacy.samples[1].selfMs = 149.0; legacy.samples[1].depth = 1;
    legacy.recordedSampleCount = 2;
    const auto merged = editor::detail::BuildRecordedProfTree(legacy);
    ASSERT_TRUE(merged.complete);
    ASSERT_EQ(merged.flat.size(), 1u);
    ASSERT_EQ(merged.nodes.size(), 2u);
    EXPECT_EQ(merged.flat[0].calls, 2);
    EXPECT_DOUBLE_EQ(merged.flat[0].totalMs, 299.0);
    EXPECT_DOUBLE_EQ(merged.flat[0].selfMs, 150.0);
    EXPECT_DOUBLE_EQ(merged.frameMs, 150.0);
    EXPECT_DOUBLE_EQ(legacy.cpuFrameElapsedMs, 150.0);
    EXPECT_DOUBLE_EQ(merged.nodes[0].totalMs, 150.0);
    EXPECT_DOUBLE_EQ(merged.nodes[1].totalMs, 149.0);

    auto renamed = legacy;
    renamed.descriptors[1].name = "LateScriptSystem::Dispatch";
    const auto separated = editor::detail::BuildRecordedProfTree(renamed);
    ASSERT_TRUE(separated.complete);
    ASSERT_EQ(separated.flat.size(), 2u);
    EXPECT_EQ(std::string(separated.flat[0].name), "LateScriptSystem");
    EXPECT_EQ(std::string(separated.flat[1].name), "LateScriptSystem::Dispatch");
    EXPECT_EQ(separated.flat[0].calls, 1); EXPECT_EQ(separated.flat[1].calls, 1);
    EXPECT_DOUBLE_EQ(separated.flat[0].totalMs, 150.0);
    EXPECT_DOUBLE_EQ(separated.flat[1].totalMs, 149.0);
    EXPECT_DOUBLE_EQ(separated.flat[0].selfMs + separated.flat[1].selfMs, 150.0);
    EXPECT_DOUBLE_EQ(separated.flat[0].totalMs + separated.flat[1].totalMs, 299.0);
    EXPECT_EQ(separated.nodes[0].key, merged.nodes[0].key);
    EXPECT_EQ(separated.flat[0].key, merged.flat[0].key);
    EXPECT_DOUBLE_EQ(separated.frameMs, merged.frameMs);
    EXPECT_DOUBLE_EQ(renamed.cpuFrameElapsedMs, legacy.cpuFrameElapsedMs);
}

} /// @note namespace fbzz::tests
