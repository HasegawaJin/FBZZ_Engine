/// @file    AnalysisProfilerGpuTests.cpp
/// @brief   Protect GPU display meaning, zero/missing states, provenance and complete interval aggregation.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Profiler/AnalysisProfilerGpu.hpp>
#include <limits>
#include <cmath>
#include <Profiler/ProfilerDisplayRows.hpp>

namespace fbzz::tests {

class AnalysisProfilerGpuTest : public testkit::EngineFixture {
protected:
    renderer::GpuProfilerSnapshot m_snapshot;

    void SetUp() override
    {
        EngineFixture::SetUp();
        m_snapshot.supported = true;
        m_snapshot.available = true;
        m_snapshot.complete = true;
        m_snapshot.physicalFrameSerial = 40;
        m_snapshot.deviceEpoch = 2;
    }

    renderer::GpuPassProfile Pass(double ms, bool available = true) const
    {
        renderer::GpuPassProfile pass;
        pass.name = "Water";
        pass.gpuMs = ms;
        pass.available = available;
        pass.physicalFrameSerial = 40;
        pass.deviceEpoch = 2;
        pass.metadata = {100, 1, 2, 3, 4, 1920, 1080, 5, 6};
        return pass;
    }
};

TEST_F(AnalysisProfilerGpuTest, PreservesMeasuredZeroWithoutInventingFrameTotal)
{
    m_snapshot.passes = {Pass(0.0)};
    const auto summary = editor::detail::SummarizeGpuDisplay(m_snapshot, 100);
    EXPECT_TRUE(summary.passSumAvailable);
    EXPECT_DOUBLE_EQ(summary.passSumMs, 0.0);
    EXPECT_FALSE(summary.totalAvailable);
    EXPECT_TRUE(summary.ageAvailable);
    EXPECT_EQ(summary.oldestAge, 0u);
    const auto groups = editor::detail::BuildGpuPassGroups(m_snapshot);
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_TRUE(groups.front().available);
    EXPECT_DOUBLE_EQ(groups.front().gpuMs, 0.0);
}

TEST_F(AnalysisProfilerGpuTest, KeepsGpuFrameTotalSeparateFromPassSum)
{
    m_snapshot.passes = {Pass(2.0), Pass(3.0)};
    m_snapshot.totalGpuMs = 20.0;
    auto summary = editor::detail::SummarizeGpuDisplay(m_snapshot, 102);
    EXPECT_FALSE(summary.totalAvailable);
    EXPECT_DOUBLE_EQ(summary.passSumMs, 5.0);
    m_snapshot.totalGpuTimeAvailable = true;
    summary = editor::detail::SummarizeGpuDisplay(m_snapshot, 102);
    EXPECT_TRUE(summary.totalAvailable);
    EXPECT_DOUBLE_EQ(summary.totalMs, 20.0);
    EXPECT_DOUBLE_EQ(summary.passSumMs, 5.0);
    m_snapshot.totalGpuMs = 0.0;
    EXPECT_TRUE(editor::detail::SummarizeGpuDisplay(m_snapshot, 102).totalAvailable);
}

TEST_F(AnalysisProfilerGpuTest, RejectsStalePayloadWhenSnapshotIsUnavailableOrUnsupported)
{
    m_snapshot.passes = {Pass(8.0)};
    m_snapshot.totalGpuTimeAvailable = true;
    m_snapshot.totalGpuMs = 12.0;
    m_snapshot.available = false;
    auto summary = editor::detail::SummarizeGpuDisplay(m_snapshot, 104);
    EXPECT_FALSE(summary.passSumAvailable);
    EXPECT_FALSE(summary.totalAvailable);
    EXPECT_FALSE(summary.ageAvailable);
    EXPECT_FALSE(editor::detail::BuildGpuPassGroups(m_snapshot).front().available);
    m_snapshot.available = true;
    m_snapshot.supported = false;
    summary = editor::detail::SummarizeGpuDisplay(m_snapshot, 104);
    EXPECT_FALSE(summary.passSumAvailable);
    EXPECT_FALSE(summary.totalAvailable);
    EXPECT_FALSE(summary.ageAvailable);
}

TEST_F(AnalysisProfilerGpuTest, AddsAllSameNameIntervalsBeforeHistoryDeduplication)
{
    m_snapshot.passes = {Pass(2.0), Pass(3.0), Pass(0.0)};
    const auto groups = editor::detail::BuildGpuPassGroups(m_snapshot);
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups.front().intervalCount, 3u);
    EXPECT_EQ(groups.front().measuredIntervals, 3u);
    EXPECT_DOUBLE_EQ(groups.front().gpuMs, 5.0);
    EXPECT_FALSE(groups.front().partial);
    const auto repeated = editor::detail::BuildGpuPassGroups(m_snapshot);
    ASSERT_EQ(repeated.size(), 1u);
    EXPECT_EQ(editor::detail::GpuPassRowKey(*groups.front().source), editor::detail::GpuPassRowKey(*repeated.front().source));
    EXPECT_DOUBLE_EQ(repeated.front().gpuMs, 5.0);
}

TEST_F(AnalysisProfilerGpuTest, MarksMissingIntervalsAndDroppedCaptureAsPartial)
{
    m_snapshot.passes = {Pass(2.0), Pass(999.0, false), Pass(3.0)};
    auto summary = editor::detail::SummarizeGpuDisplay(m_snapshot, 104);
    EXPECT_TRUE(summary.passSumAvailable);
    EXPECT_TRUE(summary.partial);
    EXPECT_DOUBLE_EQ(summary.passSumMs, 5.0);
    EXPECT_EQ(summary.measuredIntervals, 2u);
    EXPECT_EQ(summary.unavailableIntervals, 1u);
    const auto groups = editor::detail::BuildGpuPassGroups(m_snapshot);
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_TRUE(groups.front().partial);
    EXPECT_EQ(groups.front().intervalCount, 3u);
    EXPECT_DOUBLE_EQ(groups.front().gpuMs, 5.0);
    m_snapshot.passes = {Pass(0.0)};
    m_snapshot.droppedPassCount = 1;
    EXPECT_TRUE(editor::detail::SummarizeGpuDisplay(m_snapshot, 104).partial);
    m_snapshot.droppedPassCount = 0;
    m_snapshot.complete = false;
    EXPECT_TRUE(editor::detail::SummarizeGpuDisplay(m_snapshot, 104).partial);
}

TEST_F(AnalysisProfilerGpuTest, LeavesEmptyAndEntirelyMissingCapturesUnavailable)
{
    EXPECT_FALSE(editor::detail::SummarizeGpuDisplay(m_snapshot, 104).passSumAvailable);
    m_snapshot.passes = {Pass(0.0, false)};
    const auto summary = editor::detail::SummarizeGpuDisplay(m_snapshot, 104);
    EXPECT_FALSE(summary.passSumAvailable);
    EXPECT_EQ(summary.unavailableIntervals, 1u);
    EXPECT_FALSE(summary.ageAvailable);
}

TEST_F(AnalysisProfilerGpuTest, RetainsMeasuredSiblingsWhenFirstIntervalIsUnavailable)
{
    m_snapshot.passes = {Pass(999.0, false), Pass(2.0), Pass(3.0)};
    const auto groups = editor::detail::BuildGpuPassGroups(m_snapshot);
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_TRUE(groups.front().available);
    EXPECT_TRUE(groups.front().partial);
    EXPECT_EQ(groups.front().measuredIntervals, 2u);
    EXPECT_DOUBLE_EQ(groups.front().gpuMs, 5.0);
}

TEST_F(AnalysisProfilerGpuTest, KeepsEveryViewAndGenerationInGroupAndSeriesIdentity)
{
    const auto first = Pass(1.0);
    std::vector<renderer::GpuPassProfile> variants(10, first);
    variants[0].metadata.viewId++;
    variants[1].metadata.sceneGeneration++;
    variants[2].metadata.planGeneration++;
    variants[3].metadata.resourceEpoch++;
    variants[4].metadata.outputId++;
    variants[5].metadata.outputGeneration++;
    variants[6].metadata.width++;
    variants[7].metadata.height++;
    variants[8].deviceEpoch++;
    variants[9].name = "Water/1:2";
    for (const auto& variant : variants) {
        SCOPED_TRACE(editor::detail::GpuPassSeriesKey(variant));
        m_snapshot.passes = {first, variant};
        EXPECT_EQ(editor::detail::BuildGpuPassGroups(m_snapshot).size(), 2u);
        EXPECT_NE(editor::detail::GpuPassSeriesKey(first), editor::detail::GpuPassSeriesKey(variant));
        EXPECT_NE(editor::detail::GpuPassRowKey(first), editor::detail::GpuPassRowKey(variant));
    }
}

TEST_F(AnalysisProfilerGpuTest, UsesStableSeriesButDistinctRowIdentityForSourceFrames)
{
    const auto first = Pass(1.0);
    auto next = first;
    next.metadata.applicationFrameSerial++;
    EXPECT_EQ(editor::detail::GpuPassSeriesKey(first), editor::detail::GpuPassSeriesKey(next));
    EXPECT_NE(editor::detail::GpuPassRowKey(first), editor::detail::GpuPassRowKey(next));
    m_snapshot.passes = {first, next};
    EXPECT_EQ(editor::detail::BuildGpuPassGroups(m_snapshot).size(), 2u);
    next = first;
    next.physicalFrameSerial++;
    EXPECT_EQ(editor::detail::GpuPassSeriesKey(first), editor::detail::GpuPassSeriesKey(next));
    EXPECT_NE(editor::detail::GpuPassRowKey(first), editor::detail::GpuPassRowKey(next));
}

TEST_F(AnalysisProfilerGpuTest, ReportsAgeRangeAndRejectsUnknownOrFutureAge)
{
    auto older = Pass(2.0);
    older.metadata.applicationFrameSerial = 98;
    m_snapshot.passes = {Pass(3.0), older};
    auto summary = editor::detail::SummarizeGpuDisplay(m_snapshot, 104);
    EXPECT_TRUE(summary.ageAvailable);
    EXPECT_EQ(summary.youngestAge, 4u);
    EXPECT_EQ(summary.oldestAge, 6u);
    m_snapshot.passes.back().metadata.applicationFrameSerial = 105;
    EXPECT_FALSE(editor::detail::SummarizeGpuDisplay(m_snapshot, 104).ageAvailable);
    m_snapshot.passes.back().metadata.applicationFrameSerial = 0;
    EXPECT_FALSE(editor::detail::SummarizeGpuDisplay(m_snapshot, 104).ageAvailable);
    EXPECT_FALSE(editor::detail::SummarizeGpuDisplay(m_snapshot, 0).ageAvailable);
}

TEST_F(AnalysisProfilerGpuTest, RejectsInvalidDurationsAndMismatchedPhysicalSources)
{
    m_snapshot.passes = {Pass(-1.0), Pass((std::numeric_limits<double>::infinity)()),
        Pass((std::numeric_limits<double>::quiet_NaN)()), Pass(1.0), Pass(2.0)};
    m_snapshot.passes[3].physicalFrameSerial++;
    m_snapshot.passes[4].deviceEpoch++;
    const auto summary = editor::detail::SummarizeGpuDisplay(m_snapshot, 104);
    EXPECT_FALSE(summary.passSumAvailable);
    EXPECT_EQ(summary.unavailableIntervals, 5u);
    for (const auto& group : editor::detail::BuildGpuPassGroups(m_snapshot)) EXPECT_FALSE(group.available);
    m_snapshot.totalGpuTimeAvailable = true;
    m_snapshot.totalGpuMs = -1.0;
    EXPECT_FALSE(editor::detail::SummarizeGpuDisplay(m_snapshot, 104).totalAvailable);
    m_snapshot.totalGpuMs = (std::numeric_limits<double>::quiet_NaN)();
    EXPECT_FALSE(editor::detail::SummarizeGpuDisplay(m_snapshot, 104).totalAvailable);
}

TEST_F(AnalysisProfilerGpuTest, KeepsDisplayIdentityAcrossNewSourcesAndChangingIntervalCounts)
{
    m_snapshot.passes = {Pass(1.0)};
    const auto before = editor::detail::BuildGpuDisplayRows(m_snapshot);
    const auto observation = editor::detail::GpuPassRowKey(m_snapshot.passes.front());

    ++m_snapshot.physicalFrameSerial;
    for (int interval = 0; interval < 2; ++interval) {
        auto pass = Pass(3.0);
        pass.physicalFrameSerial = m_snapshot.physicalFrameSerial;
        pass.metadata.applicationFrameSerial = 101;
        if (interval == 0) m_snapshot.passes.clear();
        m_snapshot.passes.push_back(pass);
    }
    const auto after = editor::detail::BuildGpuDisplayRows(m_snapshot);

    ASSERT_EQ(before.size(), 1u);
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(before.front().key, after.front().key);
    EXPECT_NE(observation, editor::detail::GpuPassRowKey(m_snapshot.passes.front()));
    EXPECT_EQ(after.front().intervalCount, 2u);
    EXPECT_DOUBLE_EQ(after.front().gpuMs, 6.0);
}

TEST_F(AnalysisProfilerGpuTest, KeepsSameNamedContextsAndConcurrentSourceSlotsDistinct)
{
    auto older = Pass(2.0); older.metadata.applicationFrameSerial = 99;
    auto otherView = Pass(4.0); otherView.metadata.viewId = 9;
    m_snapshot.passes = {older, Pass(3.0), otherView};

    const auto rows = editor::detail::BuildGpuDisplayRows(m_snapshot);

    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].source.metadata.applicationFrameSerial, 100u);
    EXPECT_NE(rows[0].key, rows[1].key);
    EXPECT_NE(rows[0].key, rows[2].key);
    EXPECT_NE(rows[1].key, rows[2].key);
    EXPECT_EQ(rows[0].seriesKey, rows[2].seriesKey);
    EXPECT_NE(rows[0].seriesKey, rows[1].seriesKey);
}

TEST_F(AnalysisProfilerGpuTest, RetainsDelayedGpuValuesWithoutRefreshingAgeOrInventingZero)
{
    m_snapshot.passes = {Pass(0.0)};
    editor::detail::ProfilerDisplayRows<editor::detail::GpuDisplayRow> rows(512, 1024 * 1024);
    rows.Update(1, 104, "view1/device2", false, editor::detail::BuildGpuDisplayRows(m_snapshot));
    const auto summary = editor::detail::SummarizeGpuDisplay(m_snapshot, 104);

    rows.Update(1, 105, "view1/device2", false, {});

    ASSERT_EQ(rows.Rows().size(), 1u);
    EXPECT_FALSE(rows.Rows().front().present);
    EXPECT_EQ(rows.Rows().front().lastSeenFrame, 104u);
    EXPECT_TRUE(rows.Rows().front().value.available);
    EXPECT_DOUBLE_EQ(rows.Rows().front().value.gpuMs, 0.0);
    EXPECT_TRUE(summary.ageAvailable);
    EXPECT_EQ(summary.oldestAge, 4u);
    rows.Update(1, 225, "view1/device2", false, {});
    EXPECT_TRUE(rows.Rows().empty());
}

TEST_F(AnalysisProfilerGpuTest, LeavesMissingAndRepeatedGpuObservationsAsPlotGaps)
{
    const std::vector<editor::detail::GpuHistoryDisplaySample> samples = {{10, 7, 4, 0.0f}, {13, 10, 5, 8.0f}};

    const auto plot = editor::detail::BuildGpuObservationPlot(samples, {10, 11, 12, 13});

    ASSERT_EQ(plot.size(), 4u);
    EXPECT_FLOAT_EQ(plot[0].ms, 0.0f);
    EXPECT_TRUE(std::isnan(plot[1].ms));
    EXPECT_TRUE(std::isnan(plot[2].ms));
    EXPECT_EQ(plot[1].observedSerial, 11u);
    EXPECT_FLOAT_EQ(plot[3].ms, 8.0f);
    EXPECT_EQ(plot[3].applicationSerial, 10u);
}

TEST_F(AnalysisProfilerGpuTest, KeepsAllDistinctSourcesObservedAtOneCaptureInThePlot)
{
    const std::vector<editor::detail::GpuHistoryDisplaySample> samples = {{10, 7, 4, 2.0f}, {10, 8, 4, 3.0f}};

    const auto plot = editor::detail::BuildGpuObservationPlot(samples, {10, 11});

    ASSERT_EQ(plot.size(), 3u);
    EXPECT_FLOAT_EQ(plot[0].ms, 2.0f);
    EXPECT_FLOAT_EQ(plot[1].ms, 3.0f);
    EXPECT_TRUE(std::isnan(plot[2].ms));
}

} /// @note namespace fbzz::tests
