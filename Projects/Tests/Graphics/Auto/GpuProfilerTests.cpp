/// @file    GpuProfilerTests.cpp
/// @brief   GPU 提出フェンス・ビュー由来・計測完全性を固定 timestamp の純粋台帳で検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <TestKit/TestKit.hpp>
#include <Graphics/Renderer/GpuProfilerLedger.hpp>
#include <array>
#include <limits>
#include <span>

namespace fbzz::tests {
namespace {

class GpuProfilerTest : public testkit::Fixture {
protected:
    static constexpr uint64_t DEVICE_EPOCH = 17;
    static constexpr uint64_t FREQUENCY = 1000000;
    renderer::GpuProfilerLedger m_ledger{2};
    std::array<uint64_t, 2 * renderer::GpuProfilerLedger::MAX_PASSES * 2> m_timestamps{};

    void SetUp() override
    {
        testkit::Fixture::SetUp();
        m_ledger.Reset(DEVICE_EPOCH, true);
    }

    renderer::GpuProfilerViewMetadata View(uint64_t id = 11)
    {
        renderer::GpuProfilerViewMetadata metadata;
        metadata.applicationFrameSerial = 1000;
        metadata.viewId = id;
        metadata.sceneGeneration = 71;
        metadata.planGeneration = 5;
        metadata.resourceEpoch = 8;
        metadata.width = 1280;
        metadata.height = 720;
        metadata.outputId = 19;
        metadata.outputGeneration = 4;
        return metadata;
    }

    void RecordPass(const char* name, uint64_t begin, uint64_t end)
    {
        uint32_t beginIndex = UINT32_MAX;
        uint32_t endIndex = UINT32_MAX;
        ASSERT_TRUE(m_ledger.BeginPass(name, beginIndex));
        ASSERT_TRUE(m_ledger.EndPass(name, endIndex));
        ASSERT_LT(beginIndex, m_timestamps.size());
        ASSERT_LT(endIndex, m_timestamps.size());
        ASSERT_EQ(endIndex, beginIndex + 1);
        m_timestamps[beginIndex] = begin;
        m_timestamps[endIndex] = end;
    }

    void RecordFrame(uint32_t slot, uint64_t serial, const renderer::GpuProfilerViewMetadata& metadata,
        const char* name, uint64_t begin, uint64_t end, uint64_t fence)
    {
        ASSERT_TRUE(m_ledger.BeginFrame(slot, serial, DEVICE_EPOCH));
        ASSERT_TRUE(m_ledger.BeginView(metadata));
        RecordPass(name, begin, end);
        m_ledger.EndView();
        ASSERT_EQ(m_ledger.FinishFrame(), 1u);
        ASSERT_TRUE(m_ledger.SubmitFrame(slot, fence));
    }
};

TEST_F(GpuProfilerTest, StartsUnavailableAndDoesNotInventWholeFrameOrPreparationTimings)
{
    const auto& empty = m_ledger.GetSnapshot();
    EXPECT_TRUE(empty.supported);
    EXPECT_FALSE(empty.available);
    EXPECT_FALSE(empty.complete);
    EXPECT_EQ(empty.physicalFrameSerial, 0u);
    EXPECT_EQ(empty.deviceEpoch, DEVICE_EPOCH);
    EXPECT_TRUE(empty.passes.empty());
    RecordFrame(0, 1, View(), "Lighting", 100, 400, 50);
    m_ledger.Collect(50, m_timestamps, FREQUENCY);
    const auto& result = m_ledger.GetSnapshot();
    ASSERT_TRUE(result.available && result.complete);
    ASSERT_EQ(result.passes.size(), 1u);
    EXPECT_DOUBLE_EQ(result.passes[0].gpuMs, 0.3);
    EXPECT_FALSE(result.totalGpuTimeAvailable);
    EXPECT_DOUBLE_EQ(result.totalGpuMs, 0.0);
    EXPECT_FALSE(result.preparationGpuTimeAvailable);
    EXPECT_FALSE(result.perQueueGpuTimeAvailable);
}

TEST_F(GpuProfilerTest, KeepsBothViewScopesAndCapturedMetadataInOnePhysicalFrame)
{
    auto firstView = View(0);
    auto secondView = View(22);
    secondView.sceneGeneration = 72;
    secondView.planGeneration = 6;
    secondView.resourceEpoch = 9;
    secondView.width = 960;
    secondView.height = 540;
    secondView.outputId = 23;
    secondView.outputGeneration = 7;
    const auto expectedFirst = firstView;
    const auto expectedSecond = secondView;
    ASSERT_TRUE(m_ledger.BeginFrame(1, 70, DEVICE_EPOCH));
    ASSERT_TRUE(m_ledger.BeginView(firstView));
    uint32_t beginIndex = UINT32_MAX;
    uint32_t endIndex = UINT32_MAX;
    ASSERT_TRUE(m_ledger.BeginPass("Lighting", beginIndex));
    ASSERT_TRUE(m_ledger.EndPass("Lighting", endIndex));
    EXPECT_EQ(beginIndex, renderer::GpuProfilerLedger::MAX_PASSES * 2);
    EXPECT_EQ(endIndex, beginIndex + 1);
    m_timestamps[beginIndex] = 100;
    m_timestamps[endIndex] = 150;
    m_ledger.EndView();
    ASSERT_TRUE(m_ledger.BeginView(secondView));
    RecordPass("Lighting", 200, 400);
    m_ledger.EndView();
    ASSERT_EQ(m_ledger.FinishFrame(), 2u);
    ASSERT_TRUE(m_ledger.SubmitFrame(1, 500));
    firstView = {};
    secondView = {};
    m_ledger.Collect(500, m_timestamps, FREQUENCY);
    const auto& result = m_ledger.GetSnapshot();
    ASSERT_TRUE(result.available && result.complete);
    ASSERT_EQ(result.passes.size(), 2u);
    EXPECT_EQ(result.recordedPassCount, 2u);
    EXPECT_EQ(result.droppedPassCount, 0u);
    EXPECT_EQ(result.physicalFrameSerial, 70u);
    EXPECT_EQ(result.passes[0].metadata, expectedFirst);
    EXPECT_EQ(result.passes[1].metadata, expectedSecond);
    EXPECT_DOUBLE_EQ(result.passes[0].gpuMs, 0.05);
    EXPECT_DOUBLE_EQ(result.passes[1].gpuMs, 0.2);
    for (const auto& pass : result.passes) {
        EXPECT_TRUE(pass.available);
        EXPECT_EQ(pass.name, "Lighting");
        EXPECT_EQ(pass.physicalFrameSerial, 70u);
        EXPECT_EQ(pass.deviceEpoch, DEVICE_EPOCH);
    }
}

TEST_F(GpuProfilerTest, NeverPublishesRecordingOrFinishedQueriesBeforeActualSubmission)
{
    ASSERT_TRUE(m_ledger.BeginFrame(0, 1, DEVICE_EPOCH));
    ASSERT_TRUE(m_ledger.BeginView(View()));
    RecordPass("Reflection", 100, 200);
    const auto completed = std::numeric_limits<uint64_t>::max() - 1;
    m_ledger.Collect(completed, m_timestamps, FREQUENCY);
    EXPECT_FALSE(m_ledger.GetSnapshot().available);
    EXPECT_EQ(m_ledger.GetSnapshot().physicalFrameSerial, 0u);
    m_ledger.EndView();
    ASSERT_EQ(m_ledger.FinishFrame(), 1u);
    m_ledger.Collect(completed, m_timestamps, FREQUENCY);
    EXPECT_FALSE(m_ledger.GetSnapshot().available);
    EXPECT_TRUE(m_ledger.GetSnapshot().passes.empty());
    ASSERT_TRUE(m_ledger.SubmitFrame(0, 73));
    m_ledger.Collect(72, m_timestamps, FREQUENCY);
    EXPECT_FALSE(m_ledger.GetSnapshot().available);
    m_ledger.Collect(73, m_timestamps, FREQUENCY);
    EXPECT_TRUE(m_ledger.GetSnapshot().available);
    EXPECT_EQ(m_ledger.GetSnapshot().physicalFrameSerial, 1u);
}

TEST_F(GpuProfilerTest, WaitsForEachCapturedFenceAndDoesNotReuseAnUnconsumedSlot)
{
    RecordFrame(0, 1, View(11), "First", 10, 110, 100);
    EXPECT_FALSE(m_ledger.BeginFrame(0, 2, DEVICE_EPOCH));
    RecordFrame(1, 2, View(22), "Second", 200, 500, 200);
    m_ledger.Collect(99, m_timestamps, FREQUENCY);
    EXPECT_FALSE(m_ledger.GetSnapshot().available);
    m_ledger.Collect(100, m_timestamps, FREQUENCY);
    ASSERT_EQ(m_ledger.GetSnapshot().passes.size(), 1u);
    EXPECT_EQ(m_ledger.GetSnapshot().passes[0].name, "First");
    EXPECT_EQ(m_ledger.GetSnapshot().physicalFrameSerial, 1u);
    EXPECT_FALSE(m_ledger.BeginFrame(1, 3, DEVICE_EPOCH));
    m_ledger.Collect(199, m_timestamps, FREQUENCY);
    EXPECT_EQ(m_ledger.GetSnapshot().physicalFrameSerial, 1u);
    m_ledger.Collect(200, m_timestamps, FREQUENCY);
    ASSERT_EQ(m_ledger.GetSnapshot().passes.size(), 1u);
    EXPECT_EQ(m_ledger.GetSnapshot().passes[0].name, "Second");
    EXPECT_EQ(m_ledger.GetSnapshot().passes[0].metadata.viewId, 22u);
    EXPECT_EQ(m_ledger.GetSnapshot().physicalFrameSerial, 2u);
    EXPECT_TRUE(m_ledger.BeginFrame(0, 3, DEVICE_EPOCH));
}

TEST_F(GpuProfilerTest, SelectsOnlyLatestCompletedSerialRegardlessOfRingSlotOrder)
{
    RecordFrame(1, 40, View(11), "Older", 100, 200, 100);
    RecordFrame(0, 41, View(22), "Latest", 300, 800, 200);
    m_ledger.Collect(200, m_timestamps, FREQUENCY);
    const auto& result = m_ledger.GetSnapshot();
    ASSERT_TRUE(result.available && result.complete);
    ASSERT_EQ(result.passes.size(), 1u);
    EXPECT_EQ(result.physicalFrameSerial, 41u);
    EXPECT_EQ(result.recordedPassCount, 1u);
    EXPECT_EQ(result.passes[0].name, "Latest");
    EXPECT_DOUBLE_EQ(result.passes[0].gpuMs, 0.5);
    EXPECT_TRUE(m_ledger.BeginFrame(1, 42, DEVICE_EPOCH));
}

TEST_F(GpuProfilerTest, ConsumesEachSubmittedRangeOnceWithoutRereadingChangedMappedBytes)
{
    RecordFrame(0, 1, View(), "Stable", 100, 350, 100);
    m_ledger.Collect(100, m_timestamps, FREQUENCY);
    const auto first = m_ledger.GetSnapshot();
    m_timestamps[0] = 4000;
    m_timestamps[1] = 9000;
    m_ledger.Collect(1000, m_timestamps, FREQUENCY);
    const auto& retained = m_ledger.GetSnapshot();
    ASSERT_EQ(retained.passes.size(), 1u);
    EXPECT_EQ(retained.physicalFrameSerial, first.physicalFrameSerial);
    EXPECT_EQ(retained.recordedPassCount, first.recordedPassCount);
    EXPECT_DOUBLE_EQ(retained.passes[0].gpuMs, first.passes[0].gpuMs);
    EXPECT_EQ(retained.passes[0].metadata, first.passes[0].metadata);
    EXPECT_TRUE(m_ledger.BeginFrame(0, 2, DEVICE_EPOCH));
}

TEST_F(GpuProfilerTest, MarksCapacityOverflowIncompleteWithoutOverwritingRecordedQueries)
{
    ASSERT_TRUE(m_ledger.BeginFrame(0, 1, DEVICE_EPOCH));
    ASSERT_TRUE(m_ledger.BeginView(View()));
    for (uint32_t i = 0; i < renderer::GpuProfilerLedger::MAX_PASSES; ++i)
        RecordPass("Recorded", i * 100, i * 100 + 50);
    uint32_t index = 0;
    EXPECT_FALSE(m_ledger.BeginPass("Overflow", index));
    EXPECT_EQ(index, UINT32_MAX);
    EXPECT_FALSE(m_ledger.EndPass("Overflow", index));
    EXPECT_EQ(index, UINT32_MAX);
    m_ledger.EndView();
    ASSERT_EQ(m_ledger.FinishFrame(), renderer::GpuProfilerLedger::MAX_PASSES);
    ASSERT_TRUE(m_ledger.SubmitFrame(0, 100));
    m_ledger.Collect(100, m_timestamps, FREQUENCY);
    const auto& result = m_ledger.GetSnapshot();
    EXPECT_TRUE(result.available);
    EXPECT_FALSE(result.complete);
    EXPECT_EQ(result.recordedPassCount, renderer::GpuProfilerLedger::MAX_PASSES);
    EXPECT_EQ(result.droppedPassCount, 1u);
    ASSERT_EQ(result.passes.size(), renderer::GpuProfilerLedger::MAX_PASSES);
    for (const auto& pass : result.passes) {
        EXPECT_EQ(pass.name, "Recorded");
        EXPECT_TRUE(pass.available);
        EXPECT_DOUBLE_EQ(pass.gpuMs, 0.05);
    }
}

TEST_F(GpuProfilerTest, UnmatchedEndAtExactCapacityMarksOtherwiseCompleteRecordedPairsPartial)
{
    ASSERT_TRUE(m_ledger.BeginFrame(0, 1, DEVICE_EPOCH));
    ASSERT_TRUE(m_ledger.BeginView(View()));
    for (uint32_t i = 0; i < renderer::GpuProfilerLedger::MAX_PASSES; ++i)
        RecordPass("Recorded", i * 100, i * 100 + 50);
    uint32_t index = 0;
    EXPECT_FALSE(m_ledger.EndPass("Unmatched", index));
    EXPECT_EQ(index, UINT32_MAX);
    m_ledger.EndView();
    ASSERT_EQ(m_ledger.FinishFrame(), renderer::GpuProfilerLedger::MAX_PASSES);
    ASSERT_TRUE(m_ledger.SubmitFrame(0, 100));
    m_ledger.Collect(100, m_timestamps, FREQUENCY);
    const auto& result = m_ledger.GetSnapshot();
    EXPECT_TRUE(result.available);
    EXPECT_FALSE(result.complete);
    EXPECT_EQ(result.recordedPassCount, renderer::GpuProfilerLedger::MAX_PASSES);
    EXPECT_EQ(result.droppedPassCount, 0u);
    ASSERT_EQ(result.passes.size(), renderer::GpuProfilerLedger::MAX_PASSES);
    for (const auto& pass : result.passes) {
        EXPECT_EQ(pass.name, "Recorded");
        EXPECT_TRUE(pass.available);
        EXPECT_DOUBLE_EQ(pass.gpuMs, 0.05);
    }
}

TEST_F(GpuProfilerTest, KeepsValidPassesButMarksReversedTimestampsUnavailableAndPartial)
{
    ASSERT_TRUE(m_ledger.BeginFrame(0, 1, DEVICE_EPOCH));
    ASSERT_TRUE(m_ledger.BeginView(View()));
    RecordPass("Invalid", 300, 100);
    RecordPass("Valid", 400, 700);
    m_ledger.EndView();
    ASSERT_EQ(m_ledger.FinishFrame(), 2u);
    ASSERT_TRUE(m_ledger.SubmitFrame(0, 100));
    m_ledger.Collect(100, m_timestamps, FREQUENCY);
    const auto& result = m_ledger.GetSnapshot();
    EXPECT_TRUE(result.available);
    EXPECT_FALSE(result.complete);
    EXPECT_EQ(result.recordedPassCount, 2u);
    ASSERT_EQ(result.passes.size(), 2u);
    EXPECT_FALSE(result.passes[0].available);
    EXPECT_DOUBLE_EQ(result.passes[0].gpuMs, 0.0);
    EXPECT_TRUE(result.passes[1].available);
    EXPECT_DOUBLE_EQ(result.passes[1].gpuMs, 0.3);
}

TEST_F(GpuProfilerTest, TreatsZeroTimestampAndZeroDurationAsValidSubmittedMeasurements)
{
    RecordFrame(0, 1, View(), "Zero", 0, 0, 100);
    m_ledger.Collect(100, m_timestamps, FREQUENCY);
    const auto& result = m_ledger.GetSnapshot();
    ASSERT_TRUE(result.available && result.complete);
    ASSERT_EQ(result.passes.size(), 1u);
    EXPECT_TRUE(result.passes[0].available);
    EXPECT_DOUBLE_EQ(result.passes[0].gpuMs, 0.0);
}

TEST_F(GpuProfilerTest, MarksMissingTimestampOrZeroFrequencyUnavailableRatherThanAZeroMeasurement)
{
    RecordFrame(1, 1, View(), "Missing", 100, 200, 100);
    const std::span<const uint64_t> truncated{m_timestamps.data(), renderer::GpuProfilerLedger::MAX_PASSES * 2 + 1};
    m_ledger.Collect(100, truncated, FREQUENCY);
    auto result = m_ledger.GetSnapshot();
    EXPECT_FALSE(result.available);
    EXPECT_FALSE(result.complete);
    EXPECT_EQ(result.physicalFrameSerial, 1u);
    EXPECT_EQ(result.recordedPassCount, 1u);
    ASSERT_EQ(result.passes.size(), 1u);
    EXPECT_FALSE(result.passes[0].available);
    RecordFrame(0, 2, View(), "NoFrequency", 100, 200, 200);
    m_ledger.Collect(200, m_timestamps, 0);
    result = m_ledger.GetSnapshot();
    EXPECT_FALSE(result.available);
    EXPECT_FALSE(result.complete);
    EXPECT_EQ(result.physicalFrameSerial, 2u);
    ASSERT_EQ(result.passes.size(), 1u);
    EXPECT_FALSE(result.passes[0].available);
}

TEST_F(GpuProfilerTest, FailedSubmissionDiscardsOnlyItsRangeAndKeepsPublishedSnapshot)
{
    RecordFrame(0, 1, View(), "Published", 100, 200, 100);
    m_ledger.Collect(100, m_timestamps, FREQUENCY);
    ASSERT_TRUE(m_ledger.BeginFrame(1, 2, DEVICE_EPOCH));
    ASSERT_TRUE(m_ledger.BeginView(View()));
    RecordPass("Unsubmitted", 200, 600);
    m_ledger.EndView();
    ASSERT_EQ(m_ledger.FinishFrame(), 1u);
    EXPECT_FALSE(m_ledger.SubmitFrame(1, 0));
    EXPECT_FALSE(m_ledger.SubmitFrame(1, 200));
    m_ledger.Collect(1000, m_timestamps, FREQUENCY);
    const auto& result = m_ledger.GetSnapshot();
    ASSERT_EQ(result.passes.size(), 1u);
    EXPECT_EQ(result.physicalFrameSerial, 1u);
    EXPECT_EQ(result.passes[0].name, "Published");
    EXPECT_DOUBLE_EQ(result.passes[0].gpuMs, 0.1);
    EXPECT_TRUE(m_ledger.BeginFrame(1, 3, DEVICE_EPOCH));
}

TEST_F(GpuProfilerTest, RejectsInvalidFrameIdentityWithoutReplacingRecordedOrSubmittedState)
{
    EXPECT_FALSE(m_ledger.BeginFrame(2, 1, DEVICE_EPOCH));
    EXPECT_FALSE(m_ledger.BeginFrame(0, 0, DEVICE_EPOCH));
    EXPECT_FALSE(m_ledger.BeginFrame(0, 1, DEVICE_EPOCH + 1));
    ASSERT_TRUE(m_ledger.BeginFrame(0, 1, DEVICE_EPOCH));
    EXPECT_FALSE(m_ledger.BeginFrame(1, 2, DEVICE_EPOCH));
    ASSERT_TRUE(m_ledger.BeginView(View()));
    RecordPass("Original", 100, 200);
    m_ledger.EndView();
    ASSERT_EQ(m_ledger.FinishFrame(), 1u);
    ASSERT_TRUE(m_ledger.SubmitFrame(0, 100));
    EXPECT_FALSE(m_ledger.BeginFrame(1, 1, DEVICE_EPOCH));
    EXPECT_FALSE(m_ledger.SubmitFrame(0, 200));
    m_ledger.Collect(100, m_timestamps, FREQUENCY);
    EXPECT_EQ(m_ledger.GetSnapshot().physicalFrameSerial, 1u);
    EXPECT_TRUE(m_ledger.GetSnapshot().available);
}

TEST_F(GpuProfilerTest, UnclosedOrMismatchedScopesDoNotExposeUnpairedQueries)
{
    ASSERT_TRUE(m_ledger.BeginFrame(0, 1, DEVICE_EPOCH));
    auto invalidView = View();
    invalidView.width = 0;
    EXPECT_FALSE(m_ledger.BeginView(invalidView));
    ASSERT_TRUE(m_ledger.BeginView(View()));
    uint32_t index = 0;
    ASSERT_TRUE(m_ledger.BeginPass("BeginOnly", index));
    EXPECT_FALSE(m_ledger.EndPass("WrongName", index));
    EXPECT_EQ(index, UINT32_MAX);
    ASSERT_TRUE(m_ledger.BeginPass("Unclosed", index));
    m_ledger.EndView();
    EXPECT_EQ(m_ledger.FinishFrame(), 0u);
    ASSERT_TRUE(m_ledger.SubmitFrame(0, 100));
    m_ledger.Collect(100, m_timestamps, FREQUENCY);
    const auto& result = m_ledger.GetSnapshot();
    EXPECT_FALSE(result.available);
    EXPECT_FALSE(result.complete);
    EXPECT_EQ(result.recordedPassCount, 0u);
    EXPECT_EQ(result.droppedPassCount, 2u);
    EXPECT_TRUE(result.passes.empty());
}

TEST_F(GpuProfilerTest, DeviceResetDiscardsPendingAndPublishedResultsAndRequiresCurrentEpoch)
{
    RecordFrame(0, 1, View(), "Published", 100, 200, 100);
    m_ledger.Collect(100, m_timestamps, FREQUENCY);
    RecordFrame(1, 2, View(), "Pending", 200, 600, 200);
    m_ledger.Reset(DEVICE_EPOCH + 1, false);
    EXPECT_FALSE(m_ledger.GetSnapshot().supported);
    EXPECT_FALSE(m_ledger.GetSnapshot().available);
    EXPECT_TRUE(m_ledger.GetSnapshot().passes.empty());
    EXPECT_FALSE(m_ledger.BeginFrame(0, 3, DEVICE_EPOCH + 1));
    m_ledger.Collect(1000, m_timestamps, FREQUENCY);
    EXPECT_EQ(m_ledger.GetSnapshot().physicalFrameSerial, 0u);
    m_ledger.Reset(DEVICE_EPOCH + 2, true);
    EXPECT_FALSE(m_ledger.BeginFrame(0, 1, DEVICE_EPOCH));
    ASSERT_TRUE(m_ledger.BeginFrame(0, 1, DEVICE_EPOCH + 2));
    ASSERT_TRUE(m_ledger.BeginView(View()));
    RecordPass("Recovered", 100, 150);
    m_ledger.EndView();
    ASSERT_EQ(m_ledger.FinishFrame(), 1u);
    ASSERT_TRUE(m_ledger.SubmitFrame(0, 1));
    m_ledger.Collect(1, m_timestamps, FREQUENCY);
    const auto& result = m_ledger.GetSnapshot();
    ASSERT_TRUE(result.available && result.complete);
    EXPECT_EQ(result.deviceEpoch, DEVICE_EPOCH + 2);
    ASSERT_EQ(result.passes.size(), 1u);
    EXPECT_EQ(result.passes[0].deviceEpoch, DEVICE_EPOCH + 2);
    EXPECT_EQ(result.passes[0].name, "Recovered");
}

TEST_F(GpuProfilerTest, DeviceRemovalFenceSentinelInvalidatesPendingAndPublishedMeasurements)
{
    RecordFrame(0, 1, View(), "Published", 100, 200, 100);
    m_ledger.Collect(100, m_timestamps, FREQUENCY);
    RecordFrame(1, 2, View(), "Pending", 200, 600, 200);
    m_ledger.Collect(std::numeric_limits<uint64_t>::max(), m_timestamps, FREQUENCY);
    const auto& result = m_ledger.GetSnapshot();
    EXPECT_FALSE(result.supported);
    EXPECT_FALSE(result.available);
    EXPECT_FALSE(result.complete);
    EXPECT_EQ(result.physicalFrameSerial, 0u);
    EXPECT_TRUE(result.passes.empty());
    EXPECT_FALSE(m_ledger.BeginFrame(0, 3, DEVICE_EPOCH));
    m_ledger.Collect(1000, m_timestamps, FREQUENCY);
    EXPECT_FALSE(m_ledger.GetSnapshot().available);
}

TEST_F(GpuProfilerTest, CancelsOnlyAnOpenPairAndKeepsCompletedPassesBeforeAndAfterIt)
{
    EXPECT_FALSE(m_ledger.CancelOpenPass());
    ASSERT_TRUE(m_ledger.BeginFrame(0, 1, DEVICE_EPOCH));
    ASSERT_TRUE(m_ledger.BeginView(View()));
    RecordPass("BeforeSplit", 100, 200);
    uint32_t index = UINT32_MAX;
    ASSERT_TRUE(m_ledger.BeginPass("CrossedCommandLists", index));
    ASSERT_LT(index, m_timestamps.size());
    EXPECT_EQ(index, 2u);
    m_timestamps[index] = 99999;
    EXPECT_TRUE(m_ledger.CancelOpenPass());
    EXPECT_FALSE(m_ledger.CancelOpenPass());
    RecordPass("AfterSplit", 300, 500);
    m_ledger.EndView();
    EXPECT_FALSE(m_ledger.CancelOpenPass());
    ASSERT_EQ(m_ledger.FinishFrame(), 2u);
    ASSERT_TRUE(m_ledger.SubmitFrame(0, 100));
    m_ledger.Collect(100, m_timestamps, FREQUENCY);
    const auto& result = m_ledger.GetSnapshot();
    EXPECT_TRUE(result.available);
    EXPECT_FALSE(result.complete);
    EXPECT_EQ(result.recordedPassCount, 2u);
    EXPECT_EQ(result.droppedPassCount, 1u);
    ASSERT_EQ(result.passes.size(), 2u);
    EXPECT_EQ(result.passes[0].name, "BeforeSplit");
    EXPECT_EQ(result.passes[1].name, "AfterSplit");
    EXPECT_TRUE(result.passes[0].available);
    EXPECT_TRUE(result.passes[1].available);
    EXPECT_DOUBLE_EQ(result.passes[0].gpuMs, 0.1);
    EXPECT_DOUBLE_EQ(result.passes[1].gpuMs, 0.2);
}

TEST_F(GpuProfilerTest, ViewCompatibilityDistinguishesExactCaptureDelayedDisplayAndFutureSamples)
{
    auto current = View();
    auto sample = current;
    EXPECT_TRUE(renderer::IsGpuProfilerViewCompatible(sample, current));
    --sample.applicationFrameSerial;
    EXPECT_FALSE(renderer::IsGpuProfilerViewCompatible(sample, current));
    EXPECT_TRUE(renderer::IsGpuProfilerViewCompatible(sample, current, 8));
    sample.applicationFrameSerial = current.applicationFrameSerial - 8;
    EXPECT_TRUE(renderer::IsGpuProfilerViewCompatible(sample, current, 8));
    --sample.applicationFrameSerial;
    EXPECT_FALSE(renderer::IsGpuProfilerViewCompatible(sample, current, 8));
    sample.applicationFrameSerial = current.applicationFrameSerial + 1;
    EXPECT_FALSE(renderer::IsGpuProfilerViewCompatible(sample, current, std::numeric_limits<uint64_t>::max()));
    current.applicationFrameSerial = std::numeric_limits<uint64_t>::max();
    sample.applicationFrameSerial = current.applicationFrameSerial - 8;
    EXPECT_TRUE(renderer::IsGpuProfilerViewCompatible(sample, current, 8));
    --sample.applicationFrameSerial;
    EXPECT_FALSE(renderer::IsGpuProfilerViewCompatible(sample, current, 8));
    current.applicationFrameSerial = 0;
    sample.applicationFrameSerial = std::numeric_limits<uint64_t>::max();
    EXPECT_FALSE(renderer::IsGpuProfilerViewCompatible(sample, current, std::numeric_limits<uint64_t>::max()));
}

TEST_F(GpuProfilerTest, ViewCompatibilityRequiresEveryCapturedSourceFieldAndAResolvedPlan)
{
    auto current = View();
    auto sample = current;
    sample.applicationFrameSerial -= 8;
    ASSERT_TRUE(renderer::IsGpuProfilerViewCompatible(sample, current, 8));
    std::array<renderer::GpuProfilerViewMetadata, 8> incompatible;
    incompatible.fill(sample);
    ++incompatible[0].viewId;
    ++incompatible[1].sceneGeneration;
    ++incompatible[2].planGeneration;
    ++incompatible[3].resourceEpoch;
    ++incompatible[4].width;
    ++incompatible[5].height;
    ++incompatible[6].outputId;
    ++incompatible[7].outputGeneration;
    for (size_t i = 0; i < incompatible.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_FALSE(renderer::IsGpuProfilerViewCompatible(incompatible[i], current, 8));
    }
    current.planGeneration = 0;
    sample = current;
    EXPECT_FALSE(renderer::IsGpuProfilerViewCompatible(sample, current));
}

} /// @note namespace
} /// @note namespace fbzz::tests
