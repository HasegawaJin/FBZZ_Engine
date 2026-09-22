/// @file    MemoryTrackerTests.cpp
/// @brief   MemoryTracker のタグ別統計とポインタ台帳を自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/Memory/MemoryTracker.hpp>
#include <memory>
#include <vector>

namespace fbzz::tests {

class MemoryTrackerTest : public testkit::EngineFixture {};

TEST_F(MemoryTrackerTest, RecordsAndFreesAllocationInfo)
{
    core::MemoryTracker tracker;
    int value = 0;
    core::AllocationInfo info;
    info.pointer = &value;
    info.size = sizeof(value);
    info.alignment = alignof(int);
    info.tag = core::MemoryTag::CORE;
    info.allocatorName = "TestAllocator";
    info.file = "MemoryTrackerTests.cpp";
    info.line = 1;

    ASSERT_TRUE(tracker.RecordAllocation(info));
    EXPECT_EQ(tracker.GetActiveAllocationCount(), 1u);
    EXPECT_EQ(tracker.GetStats(core::MemoryTag::CORE).used, sizeof(value));
    ASSERT_NE(tracker.GetLeak(0), nullptr);
    EXPECT_EQ(tracker.GetLeak(0)->pointer, &value);

    EXPECT_TRUE(tracker.RecordFree(&value));
    EXPECT_EQ(tracker.GetActiveAllocationCount(), 0u);
    EXPECT_EQ(tracker.GetStats(core::MemoryTag::CORE).used, 0u);
}

TEST_F(MemoryTrackerTest, RejectsNullDuplicateAndUnknownFrees)
{
    core::MemoryTracker tracker;
    core::AllocationInfo info;
    int value = 0;
    info.pointer = &value;
    info.size = sizeof(value);
    info.tag = core::MemoryTag::SCENE;

    EXPECT_FALSE(tracker.RecordAllocation({}));
    ASSERT_TRUE(tracker.RecordAllocation(info));
    EXPECT_FALSE(tracker.RecordAllocation(info));
    EXPECT_FALSE(tracker.RecordFree(nullptr));
    EXPECT_FALSE(tracker.RecordFree(&info));
    EXPECT_TRUE(tracker.RecordFree(&value));
}

TEST_F(MemoryTrackerTest, MapsUnknownTagsToUnknownStatistics)
{
    core::MemoryTracker tracker;
    constexpr auto invalidTag = static_cast<core::MemoryTag>(999);

    tracker.RecordAllocation(invalidTag, 16);

    EXPECT_EQ(tracker.GetStats(core::MemoryTag::UNKNOWN).used, 16u);
    EXPECT_STREQ(tracker.GetTagName(invalidTag), "Unknown");
}

TEST_F(MemoryTrackerTest, AggregatesTagsAndClampsExcessFreesWithoutUnderflow)
{
    core::MemoryTracker tracker;
    tracker.RecordAllocation(core::MemoryTag::CORE, 16);
    tracker.RecordAllocation(core::MemoryTag::SCENE, 32);

    tracker.RecordFree(core::MemoryTag::CORE, 32);
    tracker.RecordFree(core::MemoryTag::CORE, 16);

    const auto total = tracker.GetTotalStats();
    EXPECT_EQ(total.used, 32u);
    EXPECT_EQ(total.peakUsed, 48u);
    EXPECT_EQ(total.allocationCount, 2u);
    EXPECT_EQ(total.freeCount, 2u);
    EXPECT_EQ(total.activeCount, 1u);
    EXPECT_EQ(tracker.GetStats(core::MemoryTag::CORE).used, 0u);
    EXPECT_EQ(tracker.GetStats(core::MemoryTag::CORE).activeCount, 0u);
}

TEST_F(MemoryTrackerTest, EnumeratesLiveAllocationsAcrossFreedSlots)
{
    core::MemoryTracker tracker;
    int values[3]{};
    for (auto& value : values) {
        core::AllocationInfo info;
        info.pointer = &value;
        info.size = sizeof(value);
        ASSERT_TRUE(tracker.RecordAllocation(info));
    }

    ASSERT_TRUE(tracker.RecordFree(&values[0]));

    ASSERT_NE(tracker.GetLeak(0), nullptr);
    ASSERT_NE(tracker.GetLeak(1), nullptr);
    EXPECT_EQ(tracker.GetLeak(0)->pointer, &values[1]);
    EXPECT_EQ(tracker.GetLeak(1)->pointer, &values[2]);
    EXPECT_EQ(tracker.GetLeak(2), nullptr);
}

TEST_F(MemoryTrackerTest, KeepsAggregateStatisticsWhenThePointerLedgerIsFull)
{
    auto tracker = std::make_unique<core::MemoryTracker>();
    std::vector<int> values(core::MemoryTracker::MAX_TRACKED_ALLOCATIONS + 1);
    core::AllocationInfo info;
    info.size = sizeof(int);
    info.tag = core::MemoryTag::CORE;
    for (std::size_t i = 0; i + 1 < values.size(); ++i) {
        info.pointer = &values[i];
        ASSERT_TRUE(tracker->RecordAllocation(info));
    }

    info.pointer = &values.back();
    EXPECT_FALSE(tracker->RecordAllocation(info));

    EXPECT_EQ(tracker->GetDroppedAllocationCount(), 1u);
    EXPECT_EQ(tracker->GetActiveAllocationCount(), values.size() - 1);
    EXPECT_EQ(tracker->GetTotalStats().used, values.size() * sizeof(int));
    EXPECT_FALSE(tracker->RecordFree(&values.back()));
    ASSERT_TRUE(tracker->RecordFree(&values.front()));
    tracker->RecordFree(core::MemoryTag::CORE, sizeof(int));
    ASSERT_TRUE(tracker->RecordAllocation(info));
    EXPECT_EQ(tracker->GetActiveAllocationCount(), values.size() - 1);

    tracker->Reset();
    EXPECT_EQ(tracker->GetDroppedAllocationCount(), 0u);
    EXPECT_EQ(tracker->GetActiveAllocationCount(), 0u);
    EXPECT_EQ(tracker->GetTotalStats().used, 0u);
    EXPECT_EQ(tracker->GetLeak(0), nullptr);
    ASSERT_TRUE(tracker->RecordAllocation(info));
    ASSERT_NE(tracker->GetLeak(0), nullptr);
    EXPECT_EQ(tracker->GetLeak(0)->allocationId, 1u);
}

} /// @note namespace fbzz::tests
