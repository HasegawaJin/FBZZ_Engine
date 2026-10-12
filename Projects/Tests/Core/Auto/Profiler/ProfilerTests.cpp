/// @file    ProfilerTests.cpp
/// @brief   フレーム公開、階層、収集停止の契約を時間計測値に依存せず検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <TestKit/TestKit.hpp>
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

} /// @note namespace fbzz::tests
