/// @file    ProfileRecorderTests.cpp
/// @brief   注入時計で階層、失効、障害復旧、上限と実測フレームの契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Core/Profiler/ProfileRecorder.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <vector>

namespace fbzz::tests {
namespace {
double s_profileTimeMs = 0.0;
double ReadProfileTime() { return s_profileTimeMs; }
} /// @note namespace

class ProfileRecorderTest : public testkit::EngineFixture {
protected:
    void SetUp() override { EngineFixture::SetUp(); s_profileTimeMs = 0.0; }
};

TEST_F(ProfileRecorderTest, ComputesSelfAndParentLinksWithoutDoubleCounting)
{
    profiler::ProfileRecorder recorder(true, ReadProfileTime);
    recorder.BeginFrame(42, 0.0);
    const auto parent = recorder.Begin(1);
    s_profileTimeMs = 1.0;
    const auto child = recorder.Begin(2);
    s_profileTimeMs = 3.0;
    recorder.End(child);
    s_profileTimeMs = 5.0;
    recorder.End(parent);
    recorder.EndFrame();
    const auto& snapshot = recorder.GetSnapshot();
    ASSERT_EQ(snapshot.samples.size(), 2u);
    EXPECT_EQ(snapshot.applicationFrameSerial, 42u);
    EXPECT_EQ(snapshot.samples[1].parentSampleId, snapshot.samples[0].sampleId);
    EXPECT_DOUBLE_EQ(snapshot.samples[0].inclusiveMs, 5.0);
    EXPECT_DOUBLE_EQ(snapshot.samples[0].selfMs, 3.0);
    EXPECT_DOUBLE_EQ(snapshot.samples[1].selfMs, 2.0);
    EXPECT_DOUBLE_EQ(snapshot.scopeRootSumMs, 5.0);
    EXPECT_TRUE(snapshot.complete);
    EXPECT_FALSE(snapshot.wallFrameIntervalAvailable);
}

TEST_F(ProfileRecorderTest, IgnoresForeignStaleDoubleAndNonLifoTokens)
{
    profiler::ProfileRecorder recorder(true, ReadProfileTime);
    profiler::ProfileRecorder foreign(true, ReadProfileTime);
    recorder.BeginFrame(1, 0.0);
    foreign.BeginFrame(1, 0.0);
    const auto old = recorder.Begin(1);
    const auto foreignToken = foreign.Begin(9);
    recorder.End(foreignToken);
    recorder.SetRecordingImmediate(true);
    recorder.BeginFrame(2, 0.0);
    const auto parent = recorder.Begin(2);
    const auto child = recorder.Begin(3);
    recorder.End(old);
    recorder.End(parent);
    recorder.End(child);
    recorder.End(child);
    recorder.End(parent);
    recorder.EndFrame();
    foreign.End(foreignToken);
    foreign.EndFrame();
    ASSERT_EQ(recorder.GetSnapshot().samples.size(), 2u);
    EXPECT_TRUE(recorder.GetSnapshot().complete);
}

TEST_F(ProfileRecorderTest, RecoversOnlyInnerScopesAndInvalidatesTheirTokens)
{
    profiler::ProfileRecorder recorder(true, ReadProfileTime);
    recorder.BeginFrame(1, 0.0);
    const auto outer = recorder.Begin(1);
    const auto checkpoint = recorder.Checkpoint();
    const auto interrupted = recorder.Begin(2);
    recorder.Recover(checkpoint);
    const auto next = recorder.Begin(3);
    recorder.End(interrupted);
    recorder.End(next);
    recorder.End(outer);
    recorder.EndFrame();
    const auto& snapshot = recorder.GetSnapshot();
    ASSERT_EQ(snapshot.samples.size(), 3u);
    EXPECT_EQ(snapshot.samples[1].status, profiler::ProfileSampleStatus::ABORTED);
    EXPECT_EQ(snapshot.samples[2].status, profiler::ProfileSampleStatus::COMPLETE);
    EXPECT_FALSE(snapshot.samples[0].selfAvailable);
    EXPECT_FALSE(snapshot.complete);
}

TEST_F(ProfileRecorderTest, KeepsParallelDurationsSeparateFromSelfAndInstantEvents)
{
    profiler::ProfileRecorder recorder(true, ReadProfileTime);
    recorder.BeginFrame(1, 0.0);
    const auto parent = recorder.Begin(1);
    recorder.Push(2, 20.0, profiler::ProfileSampleKind::EXTERNAL_DURATION);
    recorder.Push(3, 0.0, profiler::ProfileSampleKind::INSTANT);
    s_profileTimeMs = 5.0;
    recorder.End(parent);
    recorder.EndFrame();
    const auto& snapshot = recorder.GetSnapshot();
    EXPECT_FALSE(snapshot.samples[0].selfAvailable);
    EXPECT_FALSE(snapshot.samples[1].startAvailable);
    EXPECT_FALSE(snapshot.samples[1].selfAvailable);
    EXPECT_TRUE(snapshot.samples[2].startAvailable);
    EXPECT_DOUBLE_EQ(snapshot.samples[0].inclusiveMs, 5.0);
    EXPECT_DOUBLE_EQ(snapshot.scopeRootSumMs, 5.0);
}

TEST_F(ProfileRecorderTest, BoundsOutputWhileKeepingSavedParentAndEndCorrespondence)
{
    profiler::ProfileRecorder recorder(true, ReadProfileTime);
    recorder.BeginFrame(1, 0.0);
    const auto parent = recorder.Begin(1);
    for (size_t i = 1; i < profiler::ProfileRecorder::MAX_SAMPLES; ++i)
        recorder.Push(2, 0.0, profiler::ProfileSampleKind::INSTANT);
    const auto dropped = recorder.Begin(3);
    recorder.End(dropped);
    recorder.End(parent);
    recorder.EndFrame();
    const auto& snapshot = recorder.GetSnapshot();
    EXPECT_EQ(snapshot.samples.size(), profiler::ProfileRecorder::MAX_SAMPLES);
    EXPECT_EQ(snapshot.droppedSampleCount, 1u);
    EXPECT_FALSE(snapshot.complete);
    EXPECT_EQ(snapshot.samples[0].status, profiler::ProfileSampleStatus::COMPLETE);
    for (size_t i = 1; i < snapshot.samples.size(); ++i)
        EXPECT_EQ(snapshot.samples[i].parentSampleId, snapshot.samples[0].sampleId);
}

TEST_F(ProfileRecorderTest, RestoresSuppressionCheckpointWithoutClosingSavedAncestor)
{
    profiler::ProfileRecorder recorder(true, ReadProfileTime);
    recorder.BeginFrame(1, 0.0);
    std::vector<profiler::ProfileToken> tokens;
    for (size_t i = 0; i < profiler::ProfileRecorder::MAX_DEPTH; ++i) tokens.push_back(recorder.Begin(1));
    const auto checkpoint = recorder.Checkpoint();
    const auto lost = recorder.Begin(2);
    const auto lostChild = recorder.Begin(3);
    recorder.Recover(checkpoint);
    const auto next = recorder.Begin(4);
    recorder.End(lostChild);
    recorder.End(lost);
    recorder.End(next);
    for (auto it = tokens.rbegin(); it != tokens.rend(); ++it) recorder.End(*it);
    recorder.EndFrame();
    const auto& snapshot = recorder.GetSnapshot();
    EXPECT_EQ(snapshot.samples.size(), profiler::ProfileRecorder::MAX_DEPTH);
    EXPECT_EQ(snapshot.droppedSampleCount, 3u);
    EXPECT_FALSE(snapshot.complete);
    EXPECT_EQ(snapshot.samples.front().status, profiler::ProfileSampleStatus::COMPLETE);
}

TEST_F(ProfileRecorderTest, AppliesRecordingAndClearAtBoundariesAndCountsUnframedCalls)
{
    profiler::ProfileRecorder recorder(true, ReadProfileTime);
    recorder.Begin(1);
    recorder.BeginFrame(1, 0.0);
    const auto token = recorder.Begin(1);
    recorder.RequestRecording(false);
    EXPECT_TRUE(recorder.IsRecording());
    recorder.End(token);
    recorder.EndFrame();
    EXPECT_EQ(recorder.GetSnapshot().unframedInvocationCount, 1u);
    const auto session = recorder.GetSnapshot().captureSessionId;
    recorder.BeginFrame(2, 10.0);
    EXPECT_FALSE(recorder.IsRecording());
    EXPECT_FALSE(recorder.GetSnapshot().available);
    recorder.RequestRecording(true);
    recorder.BeginFrame(3, 20.0);
    s_profileTimeMs = 20.0;
    recorder.EndFrame();
    EXPECT_GT(recorder.GetSnapshot().captureSessionId, session);
    EXPECT_FALSE(recorder.GetSnapshot().wallFrameIntervalAvailable);
    recorder.BeginFrame(4, 30.0);
    s_profileTimeMs = 32.0;
    recorder.EndFrame();
    EXPECT_DOUBLE_EQ(recorder.GetSnapshot().wallFrameIntervalMs, 10.0);
    EXPECT_DOUBLE_EQ(recorder.GetSnapshot().cpuFrameElapsedMs, 2.0);
    recorder.RequestClear();
    recorder.BeginFrame(5, 40.0);
    s_profileTimeMs = 40.0;
    recorder.EndFrame();
    EXPECT_TRUE(recorder.GetSnapshot().samples.empty());
    EXPECT_FALSE(recorder.GetSnapshot().wallFrameIntervalAvailable);
}

TEST_F(ProfileRecorderTest, OldRaiiTokenCannotCloseScopeAfterImmediateReset)
{
    const bool enabled = profiler::Profiler::IsEnabled();
    profiler::Profiler::SetEnabled(true);
    profiler::Profiler::BeginFrame();
    {
        profiler::ProfileScope invalidated{profiler::ProfilerMarker("old")};
        profiler::Profiler::SetEnabled(false);
        profiler::Profiler::SetEnabled(true);
        profiler::Profiler::BeginFrame();
        profiler::Profiler::BeginSample(profiler::ProfilerMarker("new"));
    }
    profiler::Profiler::EndSample();
    profiler::Profiler::EndFrame();
    ASSERT_EQ(profiler::Profiler::GetSnapshot().samples.size(), 1u);
    EXPECT_TRUE(profiler::Profiler::GetSnapshot().complete);
    profiler::Profiler::SetEnabled(enabled);
}

TEST_F(ProfileRecorderTest, ForeignCheckpointDoesNotChangeFacadeManualStack)
{
    const bool enabled = profiler::Profiler::IsEnabled();
    profiler::Profiler::SetEnabled(false);
    profiler::Profiler::SetEnabled(true);
    profiler::Profiler::BeginFrame();
    profiler::Profiler::BeginSample(profiler::ProfilerMarker("parent"));
    profiler::ProfileRecorder foreign(true, ReadProfileTime);
    foreign.BeginFrame(1, 0.0);
    profiler::Profiler::Recover({.recorder = foreign.Checkpoint()});
    profiler::Profiler::EndSample();
    profiler::Profiler::EndFrame();
    ASSERT_EQ(profiler::Profiler::GetSnapshot().samples.size(), 1u);
    EXPECT_TRUE(profiler::Profiler::GetSnapshot().complete);
    foreign.EndFrame();
    profiler::Profiler::SetEnabled(enabled);
}

TEST_F(ProfileRecorderTest, ManualDepthOverflowEndsWithoutAbortingSavedScopes)
{
    const bool enabled = profiler::Profiler::IsEnabled();
    profiler::Profiler::SetEnabled(false);
    profiler::Profiler::SetEnabled(true);
    profiler::Profiler::BeginFrame();
    for (size_t i = 0; i < profiler::ProfileRecorder::MAX_DEPTH + 10; ++i)
        profiler::Profiler::BeginSample(profiler::ProfilerMarker("deep"));
    for (size_t i = 0; i < profiler::ProfileRecorder::MAX_DEPTH + 10; ++i)
        profiler::Profiler::EndSample();
    profiler::Profiler::EndFrame();
    const auto& snapshot = profiler::Profiler::GetSnapshot();
    EXPECT_EQ(snapshot.samples.size(), profiler::ProfileRecorder::MAX_DEPTH);
    EXPECT_EQ(snapshot.droppedSampleCount, 10u);
    EXPECT_EQ(snapshot.samples.front().status, profiler::ProfileSampleStatus::COMPLETE);
    EXPECT_FALSE(snapshot.complete);
    profiler::Profiler::SetEnabled(enabled);
}

TEST_F(ProfileRecorderTest, OwnsLegacyMarkerNamesBeyondCallerBufferMutation)
{
    const bool enabled = profiler::Profiler::IsEnabled();
    profiler::Profiler::SetEnabled(false);
    profiler::Profiler::SetEnabled(true);
    profiler::Profiler::BeginFrame();
    char name[] = "Owned";
    profiler::Profiler::PushMarker(profiler::ProfilerMarker(name));
    name[0] = 'X';
    profiler::Profiler::EndFrame();
    ASSERT_EQ(profiler::Profiler::GetLastFrameRecords().size(), 1u);
    EXPECT_STREQ(profiler::Profiler::GetLastFrameRecords()[0].name, "Owned");
    profiler::Profiler::SetEnabled(enabled);
}

TEST_F(ProfileRecorderTest, FacadeRecoveryPreservesCompatibilityOverflowOutsideFaultBoundary)
{
    const bool enabled = profiler::Profiler::IsEnabled();
    profiler::Profiler::SetEnabled(false);
    profiler::Profiler::SetEnabled(true);
    profiler::Profiler::BeginFrame();
    for (size_t i = 0; i < profiler::ProfileRecorder::MAX_DEPTH + 2; ++i)
        profiler::Profiler::BeginSample(profiler::ProfilerMarker("outside"));
    const auto checkpoint = profiler::Profiler::Checkpoint();
    EXPECT_EQ(checkpoint.compatStackSize, profiler::ProfileRecorder::MAX_DEPTH);
    EXPECT_EQ(checkpoint.compatSuppressedDepth, 2u);
    profiler::Profiler::BeginSample(profiler::ProfilerMarker("interrupted-compat"));
    const auto interrupted = profiler::Profiler::BeginScope(profiler::ProfilerMarker("interrupted-native"));
    profiler::Profiler::Recover(checkpoint);
    profiler::Profiler::EndScope(interrupted);
    for (size_t i = 0; i < profiler::ProfileRecorder::MAX_DEPTH + 2; ++i)
        profiler::Profiler::EndSample();
    const auto next = profiler::Profiler::BeginScope(profiler::ProfilerMarker("next-root"));
    EXPECT_EQ(next.depth, 0u);
    profiler::Profiler::EndScope(next);
    profiler::Profiler::EndFrame();
    const auto& snapshot = profiler::Profiler::GetSnapshot();
    EXPECT_EQ(snapshot.samples.size(), profiler::ProfileRecorder::MAX_DEPTH + 1);
    EXPECT_EQ(snapshot.droppedSampleCount, 4u);
    for (const auto& sample : snapshot.samples) EXPECT_EQ(sample.status, profiler::ProfileSampleStatus::COMPLETE);
    profiler::Profiler::SetEnabled(enabled);
}

TEST_F(ProfileRecorderTest, FacadeRecoverySeparatesNativeSuppressionFromCompatibilityOverflowDepth)
{
    const bool enabled = profiler::Profiler::IsEnabled();
    profiler::Profiler::SetEnabled(false);
    profiler::Profiler::SetEnabled(true);
    profiler::Profiler::BeginFrame();
    const auto nativeParent = profiler::Profiler::BeginScope(profiler::ProfilerMarker("native-parent"));
    for (size_t i = 0; i < profiler::ProfileRecorder::MAX_DEPTH + 2; ++i)
        profiler::Profiler::BeginSample(profiler::ProfilerMarker("compat-child"));
    const auto nativeSuppressed = profiler::Profiler::BeginScope(profiler::ProfilerMarker("native-suppressed"));
    const auto checkpoint = profiler::Profiler::Checkpoint();
    EXPECT_EQ(checkpoint.recorder.suppressionDepth, 4u);
    EXPECT_EQ(checkpoint.compatSuppressedDepth, 2u);
    for (size_t i = 0; i < 2; ++i) profiler::Profiler::BeginSample(profiler::ProfilerMarker("lost-compat"));
    const auto lost = profiler::Profiler::BeginScope(profiler::ProfilerMarker("lost-native"));
    profiler::Profiler::Recover(checkpoint);
    profiler::Profiler::EndScope(lost);
    profiler::Profiler::EndScope(nativeSuppressed);
    for (size_t i = 0; i < profiler::ProfileRecorder::MAX_DEPTH + 2; ++i)
        profiler::Profiler::EndSample();
    profiler::Profiler::EndScope(nativeParent);
    const auto next = profiler::Profiler::BeginScope(profiler::ProfilerMarker("next-native-root"));
    EXPECT_EQ(next.depth, 0u);
    profiler::Profiler::EndScope(next);
    profiler::Profiler::EndFrame();
    const auto& snapshot = profiler::Profiler::GetSnapshot();
    EXPECT_EQ(snapshot.samples.size(), profiler::ProfileRecorder::MAX_DEPTH + 1);
    EXPECT_EQ(snapshot.droppedSampleCount, 7u);
    for (const auto& sample : snapshot.samples) EXPECT_EQ(sample.status, profiler::ProfileSampleStatus::COMPLETE);
    profiler::Profiler::SetEnabled(enabled);
}

TEST_F(ProfileRecorderTest, FacadeRecoveryRejectsCheckpointFromCompletedOverflowBranch)
{
    const bool enabled = profiler::Profiler::IsEnabled();
    profiler::Profiler::SetEnabled(false);
    profiler::Profiler::SetEnabled(true);
    profiler::Profiler::BeginFrame();
    for (size_t i = 0; i < profiler::ProfileRecorder::MAX_DEPTH + 1; ++i)
        profiler::Profiler::BeginSample(profiler::ProfilerMarker("outside"));
    const auto stale = profiler::Profiler::Checkpoint();
    profiler::Profiler::EndSample();
    profiler::Profiler::BeginSample(profiler::ProfilerMarker("new-overflow"));
    profiler::Profiler::BeginSample(profiler::ProfilerMarker("new-overflow-child"));
    profiler::Profiler::Recover(stale);
    const auto current = profiler::Profiler::Checkpoint();
    EXPECT_EQ(current.compatSuppressedDepth, 2u);
    EXPECT_NE(current.compatOverflowScopeId, stale.compatOverflowScopeId);
    for (size_t i = 0; i < profiler::ProfileRecorder::MAX_DEPTH + 2; ++i)
        profiler::Profiler::EndSample();
    const auto next = profiler::Profiler::BeginScope(profiler::ProfilerMarker("next-root"));
    EXPECT_EQ(next.depth, 0u);
    profiler::Profiler::EndScope(next);
    profiler::Profiler::EndFrame();
    const auto& snapshot = profiler::Profiler::GetSnapshot();
    EXPECT_EQ(snapshot.samples.size(), profiler::ProfileRecorder::MAX_DEPTH + 1);
    EXPECT_EQ(snapshot.droppedSampleCount, 3u);
    for (const auto& sample : snapshot.samples) EXPECT_EQ(sample.status, profiler::ProfileSampleStatus::COMPLETE);
    profiler::Profiler::SetEnabled(enabled);
}

TEST_F(ProfileRecorderTest, RejectsClosedSuppressionCheckpointAfterNewDeeperBranch)
{
    profiler::ProfileRecorder recorder(true, ReadProfileTime);
    recorder.BeginFrame(1, 0.0);
    std::vector<profiler::ProfileToken> parents;
    for (size_t i = 0; i < profiler::ProfileRecorder::MAX_DEPTH; ++i) parents.push_back(recorder.Begin(1));
    const auto ended = recorder.Begin(2);
    const auto stale = recorder.Checkpoint();
    recorder.End(ended);
    const auto current = recorder.Begin(3);
    const auto child = recorder.Begin(4);
    EXPECT_FALSE(recorder.Recover(stale));
    recorder.End(child);
    recorder.End(current);
    for (auto it = parents.rbegin(); it != parents.rend(); ++it) recorder.End(*it);
    recorder.EndFrame();
    for (const auto& sample : recorder.GetSnapshot().samples) EXPECT_EQ(sample.status, profiler::ProfileSampleStatus::COMPLETE);
}

TEST_F(ProfileRecorderTest, RejectsEndedNestedAnchorWhileKeepingSameSuppressionRootAlive)
{
    profiler::ProfileRecorder recorder(true, ReadProfileTime);
    recorder.BeginFrame(1, 0.0);
    std::vector<profiler::ProfileToken> parents;
    for (size_t i = 0; i < profiler::ProfileRecorder::MAX_DEPTH; ++i) parents.push_back(recorder.Begin(1));
    const auto root = recorder.Begin(2);
    const auto rootCheckpoint = recorder.Checkpoint();
    const auto ended = recorder.Begin(3);
    const auto stale = recorder.Checkpoint();
    recorder.End(ended);
    const auto current = recorder.Begin(4);
    const auto child = recorder.Begin(5);
    EXPECT_FALSE(recorder.Recover(stale));
    recorder.End(child);
    recorder.End(current);
    EXPECT_TRUE(recorder.Recover(rootCheckpoint));
    recorder.End(root);
    for (auto it = parents.rbegin(); it != parents.rend(); ++it) recorder.End(*it);
    recorder.EndFrame();
    for (const auto& sample : recorder.GetSnapshot().samples) EXPECT_EQ(sample.status, profiler::ProfileSampleStatus::COMPLETE);
}

TEST_F(ProfileRecorderTest, BoundsSuppressionAnchorsAndPreservesExistingOuterRecovery)
{
    profiler::ProfileRecorder recorder(true, ReadProfileTime);
    recorder.BeginFrame(1, 0.0);
    std::vector<profiler::ProfileToken> parents;
    for (size_t i = 0; i < profiler::ProfileRecorder::MAX_DEPTH; ++i) parents.push_back(recorder.Begin(1));
    const auto root = recorder.Begin(2);
    const auto saved = recorder.Checkpoint();
    for (size_t i = 1; i < profiler::ProfileRecorder::MAX_SAMPLES; ++i) {
        recorder.Begin(3);
        EXPECT_NE(recorder.Checkpoint().recorderId, 0u);
    }
    const auto untracked = recorder.Begin(4);
    const auto capacityCheckpoint = recorder.Checkpoint();
    EXPECT_EQ(capacityCheckpoint.recorderId, 0u);
    EXPECT_FALSE(recorder.Recover(capacityCheckpoint));
    EXPECT_TRUE(recorder.Recover(saved));
    recorder.End(untracked);
    recorder.End(root);
    for (auto it = parents.rbegin(); it != parents.rend(); ++it) recorder.End(*it);
    recorder.EndFrame();
    EXPECT_EQ(recorder.GetSnapshot().samples.size(), profiler::ProfileRecorder::MAX_DEPTH);
    EXPECT_EQ(recorder.GetSnapshot().droppedSampleCount, profiler::ProfileRecorder::MAX_SAMPLES + 1);
    for (const auto& sample : recorder.GetSnapshot().samples) EXPECT_EQ(sample.status, profiler::ProfileSampleStatus::COMPLETE);
}

TEST_F(ProfileRecorderTest, BoundsOwnedStringCapacitiesAndRetiresUnusedMarkerStrings)
{
    const bool enabled = profiler::Profiler::IsEnabled();
    profiler::Profiler::SetEnabled(false);
    profiler::Profiler::SetEnabled(true);
    profiler::Profiler::BeginFrame();
    const std::string category(127, 'c');
    std::string first;
    for (size_t i = 0; i < profiler::ProfileRecorder::MAX_SAMPLES; ++i) {
        std::string name(129, 'n');
        const auto suffix = std::to_string(i);
        name.replace(0, suffix.size(), suffix);
        if (i == 0) first = name;
        profiler::Profiler::PushMarker(profiler::ProfilerMarker(name.c_str(), category.c_str()));
    }
    profiler::Profiler::EndFrame();
    size_t ownedBytes = 0;
    for (const auto& descriptor : profiler::Profiler::GetSnapshot().descriptors)
        ownedBytes += descriptor.name.capacity() + 1 + descriptor.category.capacity() + 1;
    EXPECT_LE(ownedBytes, size_t{1024 * 1024});
    EXPECT_GT(profiler::Profiler::GetSnapshot().droppedSampleCount, 0u);
    profiler::Profiler::BeginFrame();
    profiler::Profiler::PushMarker(profiler::ProfilerMarker(first.c_str(), category.c_str()));
    profiler::Profiler::EndFrame();
    EXPECT_EQ(profiler::Profiler::GetSnapshot().descriptors.size(), 1u);
    profiler::Profiler::BeginFrame();
    const std::string large(256 * 1024, 'L');
    profiler::Profiler::PushMarker(profiler::ProfilerMarker(large.c_str(), category.c_str()));
    profiler::Profiler::EndFrame();
    EXPECT_EQ(profiler::Profiler::GetSnapshot().droppedSampleCount, 0u);
    EXPECT_EQ(profiler::Profiler::GetSnapshot().descriptors.size(), 1u);
    profiler::Profiler::SetEnabled(enabled);
}

} /// @note namespace fbzz::tests
