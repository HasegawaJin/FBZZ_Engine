/// @file    ProfilerTests.cpp
/// @brief   フレーム公開、階層、収集停止の契約を時間計測値に依存せず検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <TestKit/TestKit.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Profiler/ApplicationFrameElapsedObserver.hpp>
#include <Engine/Profiler/ProfilerFrame.hpp>
#include <Engine/Profiler/ScriptProfiler.hpp>
#include <limits>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Core/Profiler/Profiler.hpp>
#include <thread>
#include <array>

namespace fbzz::tests {

using profiler::Profiler;
using profiler::ProfilerMarker;

class ProfilerTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_wasEnabled = Profiler::IsEnabled();
        m_previousFilter = Profiler::GetRequestedCaptureFilter();
        Profiler::RequestCaptureFilter({});
        Profiler::SetEnabled(false);
        Profiler::SetEnabled(true);
    }

    void TearDown() override
    {
        Profiler::RequestCaptureFilter(m_previousFilter);
        Profiler::SetEnabled(false);
        Profiler::SetEnabled(m_wasEnabled);
        EngineFixture::TearDown();
    }

private:
    bool m_wasEnabled = true;
    profiler::PerformanceCaptureFilter m_previousFilter;
};

TEST_F(ProfilerTest, PublishesSamplesOnlyWhenTheFrameEnds)
{
    Profiler::BeginFrame();
    Profiler::PushSample(ProfilerMarker("job", "worker", 123u), 2.5);
    EXPECT_TRUE(Profiler::GetLastFrameRecords().empty());

    Profiler::EndFrame();

    const auto& records = Profiler::GetLastFrameRecords();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_STREQ(records[0].name, "job");
    EXPECT_STREQ(records[0].category, "worker");
    EXPECT_EQ(records[0].color, 123u);
    EXPECT_DOUBLE_EQ(records[0].elapsedMs, 2.5);
    EXPECT_EQ(records[0].depth, 0u);
    EXPECT_EQ(records[0].frameIndex, Profiler::GetLastFrameIndex());
}

TEST_F(ProfilerTest, KeepsThePublishedSnapshotUntilTheNextFrameEnds)
{
    Profiler::BeginFrame();
    Profiler::PushMarker(ProfilerMarker("previous"));
    Profiler::EndFrame();
    const auto previousIndex = Profiler::GetLastFrameIndex();

    Profiler::BeginFrame();

    ASSERT_EQ(Profiler::GetLastFrameRecords().size(), 1u);
    EXPECT_STREQ(Profiler::GetLastFrameRecords()[0].name, "previous");
    EXPECT_EQ(Profiler::GetLastFrameIndex(), previousIndex);
    Profiler::EndFrame();
    EXPECT_TRUE(Profiler::GetLastFrameRecords().empty());
    EXPECT_EQ(Profiler::GetLastFrameIndex(), previousIndex + 1);
}

TEST_F(ProfilerTest, RecordsNestedSamplesAndInstantMarkersAtTheirStackDepth)
{
    Profiler::BeginFrame();
    Profiler::BeginSample(ProfilerMarker("parent"));
    Profiler::BeginSample(ProfilerMarker("child"));
    Profiler::PushMarker(ProfilerMarker("event", "flow", 456u));
    Profiler::PushSample(ProfilerMarker("external"), 1.25);
    Profiler::EndSample();
    Profiler::EndSample();

    Profiler::EndFrame();

    const auto& records = Profiler::GetLastFrameRecords();
    ASSERT_EQ(records.size(), 4u);
    EXPECT_STREQ(records[0].name, "event");
    EXPECT_STREQ(records[0].category, "flow");
    EXPECT_EQ(records[0].color, 456u);
    EXPECT_DOUBLE_EQ(records[0].elapsedMs, 0.0);
    EXPECT_EQ(records[0].depth, 2u);
    EXPECT_EQ(records[1].depth, 2u);
    EXPECT_DOUBLE_EQ(records[1].elapsedMs, 1.25);
    EXPECT_STREQ(records[2].name, "child");
    EXPECT_EQ(records[2].depth, 1u);
    EXPECT_STREQ(records[3].name, "parent");
    EXPECT_EQ(records[3].depth, 0u);
}

TEST_F(ProfilerTest, DisablingClearsPublishedAndPendingSamplesAndIgnoresCollection)
{
    Profiler::BeginFrame();
    Profiler::PushMarker(ProfilerMarker("published"));
    Profiler::EndFrame();
    const auto previousIndex = Profiler::GetLastFrameIndex();
    Profiler::BeginFrame();
    Profiler::BeginSample(ProfilerMarker("unfinished"));
    Profiler::PushMarker(ProfilerMarker("pending"));

    Profiler::SetEnabled(false);
    Profiler::BeginFrame();
    Profiler::BeginSample(ProfilerMarker("ignored"));
    Profiler::PushSample(ProfilerMarker("ignored"), 3.0);
    Profiler::PushMarker(ProfilerMarker("ignored"));
    Profiler::EndSample();
    Profiler::EndFrame();

    EXPECT_FALSE(Profiler::IsEnabled());
    EXPECT_TRUE(Profiler::GetLastFrameRecords().empty());
    EXPECT_EQ(Profiler::GetLastFrameIndex(), previousIndex);
    Profiler::SetEnabled(true);
    Profiler::BeginFrame();
    Profiler::EndFrame();
    EXPECT_TRUE(Profiler::GetLastFrameRecords().empty());
}

TEST_F(ProfilerTest, KeepsLegacyCompletionOrderAcrossRecursionRecoveryAndUnclosedScopes)
{
    Profiler::BeginFrame();
    const ProfilerMarker recursive("recursive", "nested", 0x12345678u);
    const auto outer = Profiler::BeginScope(recursive);
    const auto inner = Profiler::BeginScope(recursive);
    Profiler::PushSample(ProfilerMarker("duration", "worker", 7u), 7.5);
    Profiler::EndScope(inner);
    const auto checkpoint = Profiler::Checkpoint();
    const auto interrupted = Profiler::BeginScope(ProfilerMarker("interrupted"));
    Profiler::PushMarker(ProfilerMarker("inside-interrupted"));
    Profiler::Recover(checkpoint);
    Profiler::EndScope(interrupted);
    Profiler::PushMarker(ProfilerMarker("after-recovery"));
    Profiler::EndScope(outer);
    Profiler::BeginScope(ProfilerMarker("unclosed"));
    Profiler::PushMarker(ProfilerMarker("last-complete"));
    Profiler::EndFrame();

    const auto& snapshot = Profiler::GetSnapshot();
    ASSERT_EQ(snapshot.samples.size(), 8u);
    EXPECT_FALSE(snapshot.complete);
    EXPECT_EQ(snapshot.abortedSampleCount, 2u);
    EXPECT_EQ(snapshot.samples[3].status, profiler::ProfileSampleStatus::ABORTED);
    EXPECT_EQ(snapshot.samples[6].status, profiler::ProfileSampleStatus::ABORTED);
    const std::array<const char*, 6> names{"duration", "recursive", "inside-interrupted", "after-recovery", "recursive", "last-complete"};
    const std::array<uint32_t, 6> depths{2, 1, 2, 1, 0, 1};
    const std::array<size_t, 6> sampleIndices{2, 1, 4, 5, 0, 7};
    const auto& records = Profiler::GetLastFrameRecords();
    ASSERT_EQ(records.size(), names.size());
    for (size_t index = 0; index < records.size(); ++index) {
        EXPECT_STREQ(records[index].name, names[index]);
        EXPECT_EQ(records[index].depth, depths[index]);
        EXPECT_EQ(records[index].frameIndex, snapshot.frameIndex);
        EXPECT_DOUBLE_EQ(records[index].elapsedMs, snapshot.samples[sampleIndices[index]].inclusiveMs);
    }
    EXPECT_DOUBLE_EQ(records[0].elapsedMs, 7.5);
    EXPECT_STREQ(records[0].category, "worker");
    EXPECT_EQ(records[0].color, 7u);
    EXPECT_STREQ(records[1].category, "nested");
    EXPECT_EQ(records[1].color, 0x12345678u);
    EXPECT_EQ(records[4].color, 0x12345678u);
    EXPECT_EQ(Profiler::GetLastFrameRecords().size(), names.size());
    EXPECT_EQ(Profiler::GetSnapshot().samples[3].status, profiler::ProfileSampleStatus::ABORTED);
}

TEST_F(ProfilerTest, DefersLegacyReadsAcrossDescriptorRetirementWithoutChangingThePublishedFrame)
{
    const std::string category = "retirement-category-with-owned-storage";
    const auto markerName = [](size_t index) {
        return "owned-marker-" + std::to_string(index) + "-with-storage-outside-small-string";
    };
    const std::string retainedName = markerName(2048);
    Profiler::BeginFrame();
    for (size_t index = 0; index < 4100; ++index) {
        const auto name = markerName(index);
        Profiler::PushMarker(ProfilerMarker(name.c_str(), category.c_str(), 99u));
    }
    Profiler::EndFrame();
    ASSERT_EQ(Profiler::GetSnapshot().descriptors.size(), 4100u);
    EXPECT_EQ(Profiler::GetSnapshot().droppedSampleCount, 0u);

    Profiler::BeginFrame();
    Profiler::PushSample(ProfilerMarker(retainedName.c_str(), category.c_str(), 99u), 3.25);
    Profiler::EndFrame();
    const auto owned = Profiler::GetSnapshot();
    ASSERT_EQ(owned.descriptors.size(), 1u);
    const auto previousIndex = Profiler::GetLastFrameIndex();

    Profiler::BeginFrame();
    Profiler::PushMarker(ProfilerMarker("new-registration-after-retirement"));
    const auto& previous = Profiler::GetLastFrameRecords();
    ASSERT_EQ(previous.size(), 1u);
    EXPECT_STREQ(previous[0].name, retainedName.c_str());
    EXPECT_STREQ(previous[0].category, category.c_str());
    EXPECT_DOUBLE_EQ(previous[0].elapsedMs, 3.25);
    EXPECT_EQ(previous[0].color, 99u);
    EXPECT_EQ(previous[0].frameIndex, previousIndex);
    EXPECT_EQ(Profiler::GetSnapshot().descriptors[0].sampleKey, owned.descriptors[0].sampleKey);
    Profiler::EndFrame();
    ASSERT_EQ(Profiler::GetLastFrameRecords().size(), 1u);
    EXPECT_STREQ(Profiler::GetLastFrameRecords()[0].name, "new-registration-after-retirement");
    Profiler::SetEnabled(false);
    EXPECT_EQ(owned.descriptors[0].name, retainedName);
    EXPECT_EQ(owned.descriptors[0].category, category);
    EXPECT_EQ(owned.samples[0].sampleKey, owned.descriptors[0].sampleKey);
    EXPECT_TRUE(Profiler::GetLastFrameRecords().empty());
}

TEST_F(ProfilerTest, PreservesOwnedDescriptorsWhenConsecutiveFramesShareOnlyPartOfTheirNames)
{
    const ProfilerMarker first("first-owned-marker-with-heap-storage", "first-category", 1u);
    const ProfilerMarker shared("shared-owned-marker-with-heap-storage", "shared-category", 2u);
    const ProfilerMarker last("last-owned-marker-with-heap-storage", "last-category", 3u);
    Profiler::BeginFrame();
    Profiler::PushSample(first, 1.0);
    Profiler::PushSample(shared, 2.0);
    Profiler::PushSample(last, 3.0);
    Profiler::EndFrame();
    const auto original = Profiler::GetSnapshot();
    Profiler::BeginFrame();
    Profiler::PushSample(ProfilerMarker("new-owned-marker-with-heap-storage", "new-category", 4u), 4.0);
    Profiler::PushSample(shared, 5.0);
    Profiler::EndFrame();
    ASSERT_EQ(Profiler::GetSnapshot().descriptors.size(), 2u);
    EXPECT_EQ(Profiler::GetSnapshot().descriptors[0].sampleKey, original.descriptors[1].sampleKey);
    const auto& middle = Profiler::GetLastFrameRecords();
    ASSERT_EQ(middle.size(), 2u);
    EXPECT_STREQ(middle[0].name, "new-owned-marker-with-heap-storage");
    EXPECT_STREQ(middle[1].name, shared.name);
    EXPECT_DOUBLE_EQ(middle[1].elapsedMs, 5.0);
    Profiler::BeginFrame();
    Profiler::PushSample(last, 6.0);
    Profiler::PushSample(first, 7.0);
    Profiler::EndFrame();
    const auto& final = Profiler::GetLastFrameRecords();
    ASSERT_EQ(final.size(), 2u);
    EXPECT_STREQ(final[0].name, last.name);
    EXPECT_STREQ(final[1].name, first.name);
    EXPECT_EQ(final[0].color, 3u);
    EXPECT_EQ(final[1].color, 1u);
    EXPECT_DOUBLE_EQ(final[0].elapsedMs, 6.0);
    EXPECT_DOUBLE_EQ(final[1].elapsedMs, 7.0);
    EXPECT_EQ(original.descriptors[0].name, first.name);
    EXPECT_EQ(original.descriptors[1].name, shared.name);
    EXPECT_EQ(original.descriptors[2].name, last.name);
}

TEST_F(ProfilerTest, KeepsCapacityLossVisibleWhenLegacyReadsSkipAFullFrame)
{
    for (uint64_t serial = 1; serial <= 2; ++serial) {
        Profiler::BeginFrame(serial, profiler::ProfileRecorder::NowMs());
        const auto parent = Profiler::BeginScope(ProfilerMarker("capacity-parent"));
        for (size_t index = 1; index < profiler::ProfileRecorder::MAX_SAMPLES + 3; ++index)
            Profiler::PushMarker(ProfilerMarker("capacity-child"));
        Profiler::EndScope(parent);
        Profiler::EndFrame();
    }
    const auto& snapshot = Profiler::GetSnapshot();
    ASSERT_EQ(snapshot.samples.size(), profiler::ProfileRecorder::MAX_SAMPLES);
    EXPECT_FALSE(snapshot.complete);
    EXPECT_EQ(snapshot.droppedSampleCount, 3u);
    EXPECT_EQ(snapshot.capacityDroppedSampleCount, 3u);
    const auto& records = Profiler::GetLastFrameRecords();
    ASSERT_EQ(records.size(), profiler::ProfileRecorder::MAX_SAMPLES);
    EXPECT_STREQ(records.front().name, "capacity-child");
    EXPECT_EQ(records.front().depth, 1u);
    EXPECT_STREQ(records.back().name, "capacity-parent");
    EXPECT_EQ(records.back().depth, 0u);
    EXPECT_EQ(Profiler::GetLastFrameRecords().size(), records.size());
    Profiler::BeginFrame();
    Profiler::EndFrame();
    EXPECT_TRUE(Profiler::GetLastFrameRecords().empty());
    EXPECT_TRUE(Profiler::GetSnapshot().complete);
    EXPECT_EQ(Profiler::GetSnapshot().droppedSampleCount, 0u);
}

TEST_F(ProfilerTest, ReplacingAnOpenFrameKeepsThePreviousPublicationUntilTheNextExplicitEnd)
{
    Profiler::BeginFrame(1, profiler::ProfileRecorder::NowMs());
    Profiler::PushMarker(ProfilerMarker("published"));
    Profiler::EndFrame();
    const auto previousIndex = Profiler::GetLastFrameIndex();
    Profiler::BeginFrame(2, profiler::ProfileRecorder::NowMs());
    Profiler::BeginScope(ProfilerMarker("unclosed-unpublished"));
    Profiler::PushMarker(ProfilerMarker("unpublished"));
    Profiler::BeginFrame(3, profiler::ProfileRecorder::NowMs());
    ASSERT_EQ(Profiler::GetLastFrameRecords().size(), 1u);
    EXPECT_STREQ(Profiler::GetLastFrameRecords()[0].name, "published");
    EXPECT_EQ(Profiler::GetLastFrameIndex(), previousIndex);
    EXPECT_EQ(Profiler::GetSnapshot().applicationFrameSerial, 1u);
    Profiler::PushMarker(ProfilerMarker("latest"));
    Profiler::EndFrame();
    EXPECT_EQ(Profiler::GetSnapshot().applicationFrameSerial, 3u);
    EXPECT_TRUE(Profiler::GetSnapshot().complete);
    EXPECT_EQ(Profiler::GetSnapshot().abortedSampleCount, 0u);
    ASSERT_EQ(Profiler::GetSnapshot().samples.size(), 1u);
    ASSERT_EQ(Profiler::GetLastFrameRecords().size(), 1u);
    EXPECT_STREQ(Profiler::GetLastFrameRecords()[0].name, "latest");
}

TEST_F(ProfilerTest, FiltersBeforeDescriptorRegistrationAndAvoidsSyntheticCapacityOverflow)
{
    ASSERT_TRUE(Profiler::RequestCaptureFilter({"Rendering", "Extract"}));
    Profiler::BeginFrame();
    for (size_t i = 0; i < profiler::ProfileRecorder::MAX_SAMPLES + 512; ++i) {
        const auto name = "Ignored" + std::to_string(i);
        Profiler::PushMarker(ProfilerMarker(name.c_str(), "Scripts"));
    }
    Profiler::BeginSample(ProfilerMarker("excluded parent", "Rendering"));
    Profiler::PushMarker(ProfilerMarker("Extract", "Rendering"));
    Profiler::PushMarker(ProfilerMarker("extract", "Rendering"));
    Profiler::PushMarker(ProfilerMarker("Extract", "Render"));
    Profiler::EndSample();
    Profiler::EndFrame();
    const auto& snapshot = Profiler::GetSnapshot();
    ASSERT_EQ(snapshot.samples.size(), 1u);
    ASSERT_EQ(snapshot.descriptors.size(), 1u);
    EXPECT_EQ(snapshot.descriptors[0].name, "Extract");
    EXPECT_EQ(snapshot.samples[0].parentSampleId, 0u);
    EXPECT_EQ(snapshot.samples[0].depth, 0u);
    EXPECT_EQ(snapshot.filteredSampleCount, profiler::ProfileRecorder::MAX_SAMPLES + 515);
    EXPECT_EQ(snapshot.droppedSampleCount, 0u);
    EXPECT_TRUE(snapshot.complete);
}

TEST_F(ProfilerTest, AppliesFilterOnlyAtFrameBoundaryAndDoesNotChangeExistingTokens)
{
    Profiler::BeginFrame();
    const auto parent = Profiler::BeginScope(ProfilerMarker("parent", "Physics"));
    ASSERT_TRUE(Profiler::RequestCaptureFilter({"Rendering", ""}));
    Profiler::PushMarker(ProfilerMarker("still current", "Physics"));
    Profiler::EndScope(parent); Profiler::EndFrame();
    const auto session = Profiler::GetSnapshot().captureSessionId;
    EXPECT_EQ(Profiler::GetSnapshot().samples.size(), 2u);
    EXPECT_TRUE(Profiler::GetSnapshot().captureFilter.category.empty());
    Profiler::BeginFrame();
    Profiler::PushMarker(ProfilerMarker("ignored", "Physics"));
    Profiler::PushMarker(ProfilerMarker("selected", "Rendering"));
    Profiler::EndScope(parent); Profiler::EndFrame();
    EXPECT_GT(Profiler::GetSnapshot().captureSessionId, session);
    EXPECT_EQ(Profiler::GetSnapshot().samples.size(), 1u);
    EXPECT_TRUE(Profiler::GetSnapshot().complete);
    EXPECT_FALSE(Profiler::RequestCaptureFilter({std::string(129, 'C'), ""}));
    EXPECT_EQ(Profiler::GetRequestedCaptureFilter().category, "Rendering");
}

TEST_F(ProfilerTest, SharesCollectionFilterAcrossThreadsWhileKeepingStacksAndSnapshotsIndependent)
{
    ASSERT_TRUE(Profiler::RequestCaptureFilter({"Rendering", "selected"}));
    std::array<profiler::PerformanceSnapshot, 2> snapshots;
    std::array<uint64_t, 2> recorderIds{};
    const auto collect = [&](size_t index) {
        Profiler::BeginFrame(100 + index, profiler::ProfileRecorder::NowMs());
        const auto wrapper = Profiler::BeginScope(ProfilerMarker("wrapper", "Scripts"));
        const auto sample = Profiler::BeginScope(ProfilerMarker("selected", "Rendering"));
        recorderIds[index] = sample.recorderId;
        Profiler::EndScope(sample); Profiler::EndScope(wrapper); Profiler::EndFrame();
        snapshots[index] = Profiler::GetSnapshot();
    };
    std::thread first(collect, 0), second(collect, 1);
    first.join(); second.join();
    EXPECT_NE(recorderIds[0], recorderIds[1]);
    for (size_t i = 0; i < snapshots.size(); ++i) {
        ASSERT_EQ(snapshots[i].samples.size(), 1u);
        EXPECT_EQ(snapshots[i].samples[0].parentSampleId, 0u);
        EXPECT_EQ(snapshots[i].captureFilter.category, "Rendering");
        EXPECT_EQ(snapshots[i].applicationFrameSerial, 100 + i);
        EXPECT_EQ(snapshots[i].filteredSampleCount, 1u);
        EXPECT_TRUE(snapshots[i].complete);
    }
}

TEST_F(ProfilerTest, KeepsThousandsOfBoundaryFramesConsistentAcrossReadsClearRecordingAndFilterRequests)
{
    for (uint64_t serial = 1; serial <= 2048; ++serial) {
        const bool recording = serial % 97 != 0;
        const bool filtered = (serial / 128) % 2 != 0;
        ASSERT_TRUE(Profiler::RequestCaptureFilter({{}, filtered ? "AnimationPass" : ""}));
        Profiler::RequestRecording(recording);
        if (serial % 251 == 0) Profiler::RequestClear();
        Profiler::BeginFrame(serial, profiler::ProfileRecorder::NowMs());
        const auto before = Profiler::GetSnapshot();
        const auto previousCount = Profiler::GetLastFrameRecords().size();
        Profiler::PushSample(ProfilerMarker("AnimationPass", "Animation"), 5.0);
        Profiler::PushSample(ProfilerMarker("ZeroWork", "Animation"), 0.0);
        Profiler::PushMarker(ProfilerMarker("EditorWork", "General"));
        EXPECT_EQ(Profiler::GetSnapshot().applicationFrameSerial, before.applicationFrameSerial);
        EXPECT_EQ(Profiler::GetLastFrameRecords().size(), previousCount);
        Profiler::EndFrame();
        Profiler::EndFrame();
        const auto& snapshot = Profiler::GetSnapshot();
        EXPECT_EQ(snapshot.recording, recording);
        if (!recording) {
            EXPECT_FALSE(snapshot.available);
            EXPECT_TRUE(Profiler::GetLastFrameRecords().empty());
            continue;
        }
        EXPECT_TRUE(snapshot.available);
        EXPECT_TRUE(snapshot.complete);
        EXPECT_EQ(snapshot.applicationFrameSerial, serial);
        EXPECT_EQ(snapshot.recordedSampleCount, filtered ? 1u : 3u);
        EXPECT_EQ(snapshot.filteredSampleCount, filtered ? 2u : 0u);
        EXPECT_EQ(snapshot.droppedSampleCount, 0u);
        ASSERT_EQ(Profiler::GetLastFrameRecords().size(), filtered ? 1u : 3u);
        EXPECT_DOUBLE_EQ(Profiler::GetLastFrameRecords()[0].elapsedMs, 5.0);
        if (!filtered) EXPECT_DOUBLE_EQ(Profiler::GetLastFrameRecords()[1].elapsedMs, 0.0);
        EXPECT_EQ(Profiler::GetSnapshot().recordedSampleCount, snapshot.recordedSampleCount);
    }
}

TEST_F(ProfilerTest, WorkerFramePublicationNeverReplacesTheOwningThreadDuringLongRecording)
{
    ASSERT_TRUE(Profiler::RequestCaptureFilter({}));
    Profiler::BeginFrame(9000, profiler::ProfileRecorder::NowMs());
    Profiler::PushSample(ProfilerMarker("MainPayload", "Main"), 5.0);
    Profiler::EndFrame();
    profiler::PerformanceSnapshot workerSnapshot;
    std::thread worker([&] {
        for (uint64_t serial = 1; serial <= 2048; ++serial) {
            Profiler::BeginFrame(serial, profiler::ProfileRecorder::NowMs());
            Profiler::PushSample(ProfilerMarker("WorkerPayload", "Worker"), 2.0);
            Profiler::EndFrame();
        }
        workerSnapshot = Profiler::GetSnapshot();
    });
    worker.join();
    ASSERT_EQ(workerSnapshot.samples.size(), 1u);
    EXPECT_EQ(workerSnapshot.applicationFrameSerial, 2048u);
    ASSERT_EQ(workerSnapshot.descriptors.size(), 1u);
    EXPECT_EQ(workerSnapshot.descriptors[0].name, "WorkerPayload");
    EXPECT_EQ(Profiler::GetSnapshot().applicationFrameSerial, 9000u);
    ASSERT_EQ(Profiler::GetLastFrameRecords().size(), 1u);
    EXPECT_STREQ(Profiler::GetLastFrameRecords()[0].name, "MainPayload");
    EXPECT_DOUBLE_EQ(Profiler::GetLastFrameRecords()[0].elapsedMs, 5.0);
}


namespace {
using PassiveObserver = profiler::detail::ApplicationFrameElapsedObserver;
using PassiveBoundary = profiler::detail::ApplicationFrameElapsedBoundary;
profiler::detail::ApplicationFrameElapsedModeWitness PassiveOffMode()
{
    return {false, false, false, 11, 22, 33, 44};
}
} /// @note namespace

TEST_F(ProfilerTest, PassiveObserverAppliesArmAndStopAtBoundariesWithoutInactivePublication)
{
    PassiveObserver observer; const auto mode = PassiveOffMode();
    observer.Begin(1, 10.0, mode); observer.End(15.0, mode);
    EXPECT_FALSE(observer.NeedsEndClock()); EXPECT_FALSE(observer.Read().available);
    const auto session = observer.Arm(); EXPECT_TRUE(observer.WillObserveNextBegin());
    EXPECT_FALSE(observer.Read().available); EXPECT_FALSE(observer.NeedsEndClock());
    observer.Begin(2, 20.0, mode); EXPECT_TRUE(observer.NeedsEndClock()); observer.End(28.0, mode);
    ASSERT_TRUE(observer.Read().available); EXPECT_EQ(observer.Read().observerSessionId, session);
    EXPECT_DOUBLE_EQ(observer.Read().cpuFrameElapsedMs, 8.0); EXPECT_FALSE(observer.Read().wallFrameIntervalAvailable);
    observer.RequestStop(); EXPECT_FALSE(observer.WillObserveNextBegin());
    observer.Begin(3, 30.0, mode); observer.End(38.0, mode);
    EXPECT_FALSE(observer.NeedsEndClock()); EXPECT_EQ(observer.Read().publicationOrdinal, 1u);
    const auto restarted = observer.Arm(); ASSERT_GT(restarted, session);
    observer.Begin(4, 40.0, mode); observer.End(45.0, mode);
    EXPECT_EQ(observer.Read().observerSessionId, restarted); EXPECT_EQ(observer.Read().publicationOrdinal, 1u);
    EXPECT_FALSE(observer.Read().wallFrameIntervalAvailable);
}
TEST_F(ProfilerTest, PassiveObserverDistinguishesMeasuredZeroFromFirstWallAndTracksRealBeginIntervals)
{
    PassiveObserver observer; const auto mode = PassiveOffMode(); ASSERT_NE(observer.Arm(), 0u);
    observer.Begin(1, 10.0, mode); observer.End(10.0, mode);
    EXPECT_TRUE(observer.Read().cpuFrameElapsedAvailable); EXPECT_DOUBLE_EQ(observer.Read().cpuFrameElapsedMs, 0.0);
    EXPECT_FALSE(observer.Read().wallFrameIntervalAvailable);
    observer.Begin(2, 10.0, mode); observer.End(10.0, mode);
    EXPECT_TRUE(observer.Read().wallFrameIntervalAvailable); EXPECT_DOUBLE_EQ(observer.Read().wallFrameIntervalMs, 0.0);
    observer.Begin(3, 26.0, mode); observer.End(31.0, mode);
    EXPECT_DOUBLE_EQ(observer.Read().wallFrameIntervalMs, 16.0); EXPECT_DOUBLE_EQ(observer.Read().cpuFrameElapsedMs, 5.0);
}
TEST_F(ProfilerTest, PassiveObserverRejectsNonfiniteNegativeAndBackwardClockWithoutClampingToZero)
{
    PassiveObserver observer; const auto mode = PassiveOffMode(); ASSERT_NE(observer.Arm(), 0u);
    observer.Begin(1, std::numeric_limits<double>::quiet_NaN(), mode); observer.End(5.0, mode);
    EXPECT_FALSE(observer.Read().cpuFrameElapsedAvailable); EXPECT_FALSE(observer.Read().complete);
    EXPECT_EQ(observer.Read().boundary, PassiveBoundary::INVALID_CLOCK);
    observer.Begin(2, 10.0, mode); observer.End(9.0, mode);
    EXPECT_FALSE(observer.Read().cpuFrameElapsedAvailable); EXPECT_FALSE(observer.Read().complete);
    observer.Begin(3, 20.0, mode); observer.End(21.0, mode);
    EXPECT_FALSE(observer.Read().wallFrameIntervalAvailable); EXPECT_TRUE(observer.Read().complete);
    observer.Begin(4, 19.0, mode); observer.End(20.0, mode);
    EXPECT_FALSE(observer.Read().wallFrameIntervalAvailable); EXPECT_FALSE(observer.Read().complete);
    EXPECT_EQ(observer.Read().boundary, PassiveBoundary::INVALID_CLOCK);
    observer.Begin(5, 30.0, mode); observer.End(std::numeric_limits<double>::infinity(), mode);
    EXPECT_FALSE(observer.Read().cpuFrameElapsedAvailable);
}
TEST_F(ProfilerTest, PassiveObserverRetainsAbortedScalarAndDoesNotPublishUnmatchedOrDoubleEnd)
{
    PassiveObserver observer; const auto mode = PassiveOffMode(); ASSERT_NE(observer.Arm(), 0u);
    observer.End(10.0, mode); EXPECT_FALSE(observer.Read().available);
    observer.Begin(1, 10.0, mode); observer.MarkAborted(); observer.End(14.0, mode);
    EXPECT_TRUE(observer.Read().cpuFrameElapsedAvailable); EXPECT_DOUBLE_EQ(observer.Read().cpuFrameElapsedMs, 4.0);
    EXPECT_FALSE(observer.Read().complete); EXPECT_EQ(observer.Read().boundary, PassiveBoundary::WINDOW_CLOSE);
    observer.End(19.0, mode); EXPECT_EQ(observer.Read().publicationOrdinal, 1u);
    observer.Begin(2, 26.0, mode); observer.End(27.0, mode);
    EXPECT_FALSE(observer.Read().wallFrameIntervalAvailable);
}
TEST_F(ProfilerTest, PassiveObserverExplainsReplacedBeginWithoutFabricatingEndWitnessOrWallContinuity)
{
    PassiveObserver observer; const auto mode = PassiveOffMode(); ASSERT_NE(observer.Arm(), 0u);
    observer.Begin(1, 10.0, mode); observer.Begin(2, 26.0, mode);
    EXPECT_EQ(observer.Read().boundary, PassiveBoundary::REPLACED_BEGIN);
    EXPECT_FALSE(observer.Read().cpuFrameElapsedAvailable); EXPECT_FALSE(observer.Read().endModeAvailable);
    EXPECT_FALSE(observer.Read().complete); EXPECT_EQ(observer.Read().publicationOrdinal, 1u);
    observer.End(30.0, mode); EXPECT_EQ(observer.Read().publicationOrdinal, 2u);
    EXPECT_EQ(observer.Read().applicationFrameSerial, 2u); EXPECT_FALSE(observer.Read().wallFrameIntervalAvailable);
}
TEST_F(ProfilerTest, PassiveCommonBoundaryPublishesFreshOffFramesWithoutUpdatingStoppedNativeSnapshot)
{
    struct RestoreBoundary {
        bool script = scene::ScriptProfiler::IsRecording();
        ~RestoreBoundary() {
            profiler::detail::RequestStopApplicationFrameElapsedObserver();
            scene::ScriptProfiler::RequestRecording(script);
            profiler::BeginApplicationProfileFrame(); profiler::EndApplicationProfileFrame();
        }
    } restore;
    Profiler::SetEnabled(false); scene::ScriptProfiler::RequestRecording(false);
    profiler::BeginApplicationProfileFrame(); profiler::EndApplicationProfileFrame();
    const uint64_t nativeIndex = Profiler::GetLastFrameIndex();
    const auto session = profiler::detail::ArmApplicationFrameElapsedObserver(); ASSERT_NE(session, 0u);
    profiler::BeginApplicationProfileFrame(); profiler::EndApplicationProfileFrame();
    const auto first = profiler::detail::ReadApplicationFrameElapsedObserver();
    ASSERT_TRUE(first.available); EXPECT_EQ(first.observerSessionId, session); EXPECT_EQ(first.publicationOrdinal, 1u);
    EXPECT_TRUE(first.beginMode.IsOff()); EXPECT_TRUE(first.endMode.IsOff()); EXPECT_TRUE(first.cpuFrameElapsedAvailable);
    EXPECT_FALSE(first.wallFrameIntervalAvailable); EXPECT_FALSE(Profiler::GetSnapshot().available);
    EXPECT_TRUE(Profiler::GetSnapshot().samples.empty()); EXPECT_EQ(Profiler::GetLastFrameIndex(), nativeIndex);
    profiler::BeginApplicationProfileFrame(); profiler::EndApplicationProfileFrame();
    const auto second = profiler::detail::ReadApplicationFrameElapsedObserver();
    EXPECT_EQ(second.publicationOrdinal, 2u); EXPECT_GT(second.applicationFrameSerial, first.applicationFrameSerial);
    EXPECT_TRUE(second.wallFrameIntervalAvailable); EXPECT_EQ(Profiler::GetLastFrameIndex(), nativeIndex);
}


namespace {
void PassiveReviewBeginCommonFrame()
{
    if (auto* resources = renderer::ResourceManager::Active()) resources->AdvanceFrame();
    profiler::BeginApplicationProfileFrame();
}
struct PassiveReviewBoundaryGuard {
    bool script = scene::ScriptProfiler::IsRecording();
    PassiveReviewBoundaryGuard() {
        Profiler::SetEnabled(false); scene::ScriptProfiler::RequestRecording(false);
        PassiveReviewBeginCommonFrame(); profiler::EndApplicationProfileFrame();
    }
    ~PassiveReviewBoundaryGuard() {
        profiler::detail::RequestStopApplicationFrameElapsedObserver();
        scene::ScriptProfiler::RequestRecording(script);
        PassiveReviewBeginCommonFrame(); profiler::EndApplicationProfileFrame();
    }
};
} /// @note namespace

TEST_F(ProfilerTest, PassivePendingArmKeepsOpenFrameAndOldLatestUntilNextCommonBegin)
{
    PassiveReviewBoundaryGuard restore;
    const auto firstSession = profiler::detail::ArmApplicationFrameElapsedObserver(); ASSERT_NE(firstSession, 0u);
    PassiveReviewBeginCommonFrame(); profiler::EndApplicationProfileFrame();
    const auto first = profiler::detail::ReadApplicationFrameElapsedObserver(); ASSERT_EQ(first.observerSessionId, firstSession);
    PassiveReviewBeginCommonFrame();
    const auto nextSession = profiler::detail::ArmApplicationFrameElapsedObserver(); ASSERT_GT(nextSession, firstSession);
    EXPECT_EQ(profiler::detail::ReadApplicationFrameElapsedObserver().observerSessionId, firstSession);
    EXPECT_EQ(profiler::detail::ReadApplicationFrameElapsedObserver().publicationOrdinal, 1u);
    profiler::EndApplicationProfileFrame();
    const auto oldCompleted = profiler::detail::ReadApplicationFrameElapsedObserver();
    EXPECT_EQ(oldCompleted.observerSessionId, firstSession); EXPECT_EQ(oldCompleted.publicationOrdinal, 2u);
    EXPECT_TRUE(oldCompleted.beginMode.IsOff()); EXPECT_TRUE(oldCompleted.endMode.IsOff());
    PassiveReviewBeginCommonFrame(); EXPECT_FALSE(profiler::detail::ReadApplicationFrameElapsedObserver().available);
    profiler::EndApplicationProfileFrame();
    const auto restarted = profiler::detail::ReadApplicationFrameElapsedObserver();
    EXPECT_EQ(restarted.observerSessionId, nextSession); EXPECT_EQ(restarted.publicationOrdinal, 1u);
    EXPECT_GT(restarted.applicationFrameSerial, oldCompleted.applicationFrameSerial);
    EXPECT_TRUE(restarted.cpuFrameElapsedAvailable); EXPECT_FALSE(restarted.wallFrameIntervalAvailable);
    EXPECT_FALSE(Profiler::GetSnapshot().available); EXPECT_TRUE(Profiler::GetSnapshot().samples.empty());
}
TEST_F(ProfilerTest, PassivePendingStopClosesOpenFrameThenLeavesLatestUnchangedWhileInactive)
{
    PassiveReviewBoundaryGuard restore;
    const auto session = profiler::detail::ArmApplicationFrameElapsedObserver(); ASSERT_NE(session, 0u);
    PassiveReviewBeginCommonFrame(); profiler::EndApplicationProfileFrame();
    PassiveReviewBeginCommonFrame(); profiler::detail::RequestStopApplicationFrameElapsedObserver();
    EXPECT_EQ(profiler::detail::ReadApplicationFrameElapsedObserver().publicationOrdinal, 1u);
    profiler::EndApplicationProfileFrame();
    const auto finalOpenFrame = profiler::detail::ReadApplicationFrameElapsedObserver();
    EXPECT_EQ(finalOpenFrame.observerSessionId, session); EXPECT_EQ(finalOpenFrame.publicationOrdinal, 2u);
    EXPECT_TRUE(finalOpenFrame.complete); EXPECT_TRUE(finalOpenFrame.endMode.IsOff());
    PassiveReviewBeginCommonFrame(); profiler::EndApplicationProfileFrame();
    const auto stopped = profiler::detail::ReadApplicationFrameElapsedObserver();
    EXPECT_EQ(stopped.observerSessionId, session); EXPECT_EQ(stopped.publicationOrdinal, 2u);
    EXPECT_EQ(stopped.applicationFrameSerial, finalOpenFrame.applicationFrameSerial);
    const auto restartedSession = profiler::detail::ArmApplicationFrameElapsedObserver(); ASSERT_GT(restartedSession, session);
    EXPECT_EQ(profiler::detail::ReadApplicationFrameElapsedObserver().observerSessionId, session);
    PassiveReviewBeginCommonFrame(); profiler::EndApplicationProfileFrame();
    const auto restarted = profiler::detail::ReadApplicationFrameElapsedObserver();
    EXPECT_EQ(restarted.observerSessionId, restartedSession); EXPECT_EQ(restarted.publicationOrdinal, 1u);
    EXPECT_FALSE(restarted.wallFrameIntervalAvailable); EXPECT_FALSE(Profiler::GetSnapshot().recording);
}

} /// @note namespace fbzz::tests
