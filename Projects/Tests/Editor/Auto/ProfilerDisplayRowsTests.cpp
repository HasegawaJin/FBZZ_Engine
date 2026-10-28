/// @file    ProfilerDisplayRowsTests.cpp
/// @brief   Protect bounded missing-row retention, identity, Freeze boundaries and paced user ordering.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Profiler/ProfilerDisplayRows.hpp>
#include <Profiler/AnalysisProfilerCpu.hpp>
#include <array>
#include <limits>

namespace fbzz::tests {
namespace {
struct DisplayRow {
    std::string key;
    std::string name;
    double ms = 0.0;
    bool available = true;
    size_t DynamicBytes() const { return key.capacity() + name.capacity() + 2; }
};
using Rows = editor::detail::ProfilerDisplayRows<DisplayRow>;
} /// @note namespace

class ProfilerDisplayRowsTest : public testkit::EngineFixture {
protected:
    Rows m_rows;
    editor::detail::ProfilerDisplayOrder m_order;

    void Order(std::vector<DisplayRow>& rows, double nowMs, bool ascending = false, bool changed = false)
    {
        m_order.Apply(rows, [](const DisplayRow& row) -> const std::string& { return row.key; },
            [=](const DisplayRow& a, const DisplayRow& b) { return ascending ? a.ms < b.ms : a.ms > b.ms; },
            ascending ? 1 : 0, nowMs, changed);
    }
};

TEST_F(ProfilerDisplayRowsTest, KeepsMeasuredZeroDistinctFromMissingAndUnavailable)
{
    m_rows.Update(1, 100, "scene", false, {{"a", "Same", 0.0, true}, {"b", "Same", 0.0, false}});

    m_rows.Update(1, 101, "scene", false, {});

    ASSERT_EQ(m_rows.Rows().size(), 2u);
    EXPECT_FALSE(m_rows.Rows()[0].present);
    EXPECT_EQ(m_rows.Rows()[0].lastSeenFrame, 100u);
    EXPECT_TRUE(m_rows.Rows()[0].value.available);
    EXPECT_DOUBLE_EQ(m_rows.Rows()[0].value.ms, 0.0);
    EXPECT_FALSE(m_rows.Rows()[1].value.available);
}

TEST_F(ProfilerDisplayRowsTest, PreservesIdentityAndPositionAcrossAlternatingAbsence)
{
    m_rows.Update(1, 1, "scene", false, {{"a", "Same", 2.0}});
    m_rows.Update(1, 2, "scene", false, {{"b", "Same", 3.0}});

    m_rows.Update(1, 3, "scene", false, {{"a", "Same", 80.0}});

    ASSERT_EQ(m_rows.Rows().size(), 2u);
    EXPECT_EQ(m_rows.Rows()[0].value.key, "a");
    EXPECT_EQ(m_rows.Rows()[1].value.key, "b");
    EXPECT_TRUE(m_rows.Rows()[0].present);
    EXPECT_FALSE(m_rows.Rows()[1].present);
    EXPECT_DOUBLE_EQ(m_rows.Rows()[0].value.ms, 80.0);
}

TEST_F(ProfilerDisplayRowsTest, ExpiresAfterTheInclusiveFrameTtlWithoutRetargetingTheSelectedKey)
{
    const std::string selected = "a";
    m_rows.Update(1, 100, "scene", false, {{selected, "Same", 2.0}});
    m_rows.Update(1, 220, "scene", false, {});
    ASSERT_EQ(m_rows.Rows().size(), 1u);

    m_rows.Update(1, 221, "scene", false, {{"b", "Same", 3.0}});

    ASSERT_EQ(m_rows.Rows().size(), 1u);
    EXPECT_NE(m_rows.Rows().front().value.key, selected);
    EXPECT_EQ(m_rows.Rows().front().value.key, "b");
}

TEST_F(ProfilerDisplayRowsTest, DoesNotAgeOrReplaceTheSamePausedCapture)
{
    m_rows.Update(1, 100, "scene", false, {{"a", "A", 2.0}});

    for (int draw = 0; draw < 1000; ++draw) m_rows.Update(1, 100, "scene", false, {});

    ASSERT_EQ(m_rows.Rows().size(), 1u);
    EXPECT_TRUE(m_rows.Rows().front().present);
    EXPECT_EQ(m_rows.Rows().front().lastSeenFrame, 100u);
}

TEST_F(ProfilerDisplayRowsTest, ShowsOnlyTheSelectedCaptureOnFreezeSeekAndResume)
{
    m_rows.Update(1, 100, "scene", false, {{"future", "Future", 20.0}});
    const auto generation = m_rows.Generation();
    m_rows.Update(1, 90, "scene", true, {{"past", "Past", 3.0}});
    ASSERT_EQ(m_rows.Rows().size(), 1u);
    EXPECT_EQ(m_rows.Rows().front().value.key, "past");
    EXPECT_EQ(m_rows.Generation(), generation);

    m_rows.Update(1, 101, "scene", false, {{"resumed", "Resumed", 8.0}});

    ASSERT_EQ(m_rows.Rows().size(), 1u);
    EXPECT_EQ(m_rows.Rows().front().value.key, "resumed");
    EXPECT_EQ(m_rows.Generation(), generation);
}

TEST_F(ProfilerDisplayRowsTest, ClearsOldRowsAndWidgetIdentityWhenSessionChanges)
{
    m_rows.Update(1, 100, "scene", false, {{"a", "A", 2.0}});
    const auto generation = m_rows.Generation();

    m_rows.Update(2, 101, "scene", false, {{"b", "B", 3.0}});

    ASSERT_EQ(m_rows.Rows().size(), 1u);
    EXPECT_EQ(m_rows.Rows().front().value.key, "b");
    EXPECT_GT(m_rows.Generation(), generation);
}

TEST_F(ProfilerDisplayRowsTest, ClearsRetainedRowsWhenSceneOrViewContextChanges)
{
    m_rows.Update(1, 100, "scene1/view1", false, {{"a", "A", 2.0}});
    m_rows.Update(1, 101, "scene2/view1", false, {});
    EXPECT_TRUE(m_rows.Rows().empty());
    m_rows.Update(1, 102, "scene2/view1", false, {{"b", "B", 3.0}});

    m_rows.Update(1, 103, "scene2/view2", false, {});

    EXPECT_TRUE(m_rows.Rows().empty());
}

TEST_F(ProfilerDisplayRowsTest, MakesExplicitClearEffectiveEvenAtTheSameFrame)
{
    m_rows.Update(1, 100, "scene", false, {{"a", "A", 2.0}});

    m_rows.Clear();
    m_rows.Update(1, 100, "scene", false, {});

    EXPECT_TRUE(m_rows.Rows().empty());
}

TEST_F(ProfilerDisplayRowsTest, BoundsRowCountAndMakesRoomAfterExpiry)
{
    Rows rows(2, 16 * 1024);
    const std::vector<DisplayRow> incoming = {{"a", "A", 1.0}, {"b", "B", 2.0}, {"c", "C", 3.0}};

    rows.Update(1, 1, "scene", false, incoming);

    ASSERT_EQ(rows.Rows().size(), 2u);
    EXPECT_TRUE(rows.Limited());
    EXPECT_EQ(incoming.size(), 3u);
    rows.Update(1, 122, "scene", false, {incoming.back()});
    ASSERT_EQ(rows.Rows().size(), 1u);
    EXPECT_EQ(rows.Rows().front().value.key, "c");
    EXPECT_FALSE(rows.Limited());
}

TEST_F(ProfilerDisplayRowsTest, RejectsOversizedOwnedStringsWithoutInventingARow)
{
    Rows rows(2, 2048);

    rows.Update(1, 1, "scene", false, {{"large", std::string(4096, 'x'), 4.0}});

    EXPECT_TRUE(rows.Rows().empty());
    EXPECT_TRUE(rows.Limited());
}

TEST_F(ProfilerDisplayRowsTest, UpdatesValuesImmediatelyAndPacesOnlyDisplayRank)
{
    std::vector<DisplayRow> rows = {{"a", "A", 1.0}, {"b", "B", 2.0}};
    Order(rows, 0.0);
    ASSERT_EQ(rows.front().key, "b");
    rows = {{"a", "A", 80.0}, {"b", "B", 2.0}};

    Order(rows, 50.0);

    EXPECT_EQ(rows.front().key, "b");
    EXPECT_DOUBLE_EQ(rows.back().ms, 80.0);
    Order(rows, 100.0);
    EXPECT_EQ(rows.front().key, "a");
}

TEST_F(ProfilerDisplayRowsTest, AppliesUserSortChangesBeforeTheRefreshDeadline)
{
    std::vector<DisplayRow> rows = {{"a", "A", 1.0}, {"b", "B", 2.0}};
    Order(rows, 0.0);

    Order(rows, 10.0, true);

    EXPECT_EQ(rows.front().key, "a");
    Order(rows, 20.0, false, true);
    EXPECT_EQ(rows.front().key, "b");
}

TEST_F(ProfilerDisplayRowsTest, KeepsTieOrderStableWhenInputEnumerationChanges)
{
    std::vector<DisplayRow> rows = {{"b", "Same", 2.0}, {"a", "Same", 2.0}};
    Order(rows, 0.0);
    rows = {{"a", "Same", 2.0}, {"b", "Same", 2.0}};

    Order(rows, 100.0);

    EXPECT_EQ(rows.front().key, "b");
}

TEST_F(ProfilerDisplayRowsTest, KeepsSameNamedCpuScopesUnderDifferentParentsDistinctAcrossRecordIds)
{
    profiler::PerformanceSnapshot snapshot;
    snapshot.available = true; snapshot.complete = true;
    snapshot.descriptors = {{1, "Parent A", "Rendering"}, {2, "Shared", "Rendering"},
        {3, "Parent B", "Rendering"}, {4, "Shared", "Rendering"}};
    for (uint64_t id = 1; id <= 4; ++id) {
        profiler::ProfileSample sample;
        sample.sampleKey = id; sample.sampleId = id;
        sample.parentSampleId = id == 2 ? 1 : id == 4 ? 3 : 0;
        sample.inclusiveMs = id % 2 == 0 ? 1.0 : 5.0; sample.selfMs = 1.0;
        snapshot.samples.push_back(sample);
    }
    const auto before = editor::detail::BuildRecordedProfTree(snapshot);

    for (auto& descriptor : snapshot.descriptors) descriptor.sampleKey += 100;
    for (auto& sample : snapshot.samples) {
        sample.sampleKey += 100; sample.sampleId += 100;
        if (sample.parentSampleId != 0) sample.parentSampleId += 100;
    }
    const auto after = editor::detail::BuildRecordedProfTree(snapshot);

    ASSERT_EQ(before.nodes.size(), 4u);
    ASSERT_EQ(after.nodes.size(), 4u);
    EXPECT_STREQ(before.nodes[1].name, before.nodes[3].name);
    EXPECT_NE(before.nodes[1].key, before.nodes[3].key);
    EXPECT_EQ(before.nodes[1].key, after.nodes[1].key);
    EXPECT_EQ(before.nodes[3].key, after.nodes[3].key);
}

TEST_F(ProfilerDisplayRowsTest, KeepsParentTopologyForContiguousSparseDuplicateAndUnorderedSampleIds)
{
    const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
    const std::array<std::array<uint64_t, 3>, 7> cases{{
        {100, 101, 102}, {100, 1000, 1005}, {100, 100, 101}, {101, 100, 102},
        {maximum - 2, maximum - 1, maximum}, {maximum, 1, 2}, {0, 1, 2}}};
    for (const auto& ids : cases) {
        SCOPED_TRACE(ids[0]);
        profiler::PerformanceSnapshot snapshot;
        snapshot.available = true; snapshot.complete = true;
        snapshot.descriptors = {{1, "First", "Rendering"}, {2, "Second", "Rendering"}, {3, "Child", "Rendering"}};
        for (size_t index = 0; index < ids.size(); ++index) {
            profiler::ProfileSample sample;
            sample.sampleId = ids[index]; sample.sampleKey = index + 1;
            sample.parentSampleId = index == 2 ? ids[1] : 0;
            sample.inclusiveMs = sample.selfMs = 1.0;
            snapshot.samples.push_back(sample);
        }
        const auto tree = editor::detail::BuildRecordedProfTree(snapshot);
        ASSERT_TRUE(tree.complete);
        ASSERT_EQ(tree.nodes.size(), 3u);
        EXPECT_EQ(tree.roots, (std::vector<int>{0, 1}));
        EXPECT_TRUE(tree.nodes[0].children.empty());
        EXPECT_EQ(tree.nodes[1].children, (std::vector<int>{2}));
        EXPECT_EQ(tree.nodes[2].depth, 1u);
        EXPECT_DOUBLE_EQ(tree.nodes[2].totalMs, 1.0);
    }
}

TEST_F(ProfilerDisplayRowsTest, DoesNotPublishSkippedContiguousSamplesAsParents)
{
    for (const uint64_t skippedParent : {uint64_t{0}, uint64_t{2}, uint64_t{3}, uint64_t{99}}) {
        SCOPED_TRACE(skippedParent);
        profiler::PerformanceSnapshot snapshot;
        snapshot.available = true; snapshot.complete = true;
        snapshot.descriptors = {{1, "First", "Rendering"}, {2, "Skipped", "Rendering"}, {3, "Child", "Rendering"}};
        for (uint64_t id = 1; id <= 3; ++id) {
            profiler::ProfileSample sample;
            sample.sampleId = sample.sampleKey = id;
            sample.inclusiveMs = sample.selfMs = 1.0;
            if (id == 2) {
                if (skippedParent == 0) sample.sampleKey = 99;
                else sample.parentSampleId = skippedParent;
            }
            if (id == 3) sample.parentSampleId = 2;
            snapshot.samples.push_back(sample);
        }
        const auto tree = editor::detail::BuildRecordedProfTree(snapshot);
        EXPECT_FALSE(tree.complete);
        ASSERT_EQ(tree.nodes.size(), 1u);
        EXPECT_STREQ(tree.nodes.front().name, "First");
        EXPECT_TRUE(tree.nodes.front().children.empty());
    }
}

} /// @note namespace fbzz::tests
