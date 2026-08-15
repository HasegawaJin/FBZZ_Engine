// FBZZ Engine
// MemoryTrackerTests.cpp | GoogleTest
// MemoryTracker のタグ別統計とポインタ台帳を自動検証する。
#include <gtest/gtest.h>

#include <Engine/Core/Memory/MemoryTracker.hpp>

namespace fbzz::tests {

TEST(MemoryTrackerTest, RecordsAndFreesAllocationInfo)
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

TEST(MemoryTrackerTest, RejectsNullDuplicateAndUnknownFrees)
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

TEST(MemoryTrackerTest, MapsUnknownTagsToUnknownStatistics)
{
    core::MemoryTracker tracker;
    constexpr auto invalidTag = static_cast<core::MemoryTag>(999);

    tracker.RecordAllocation(invalidTag, 16);

    EXPECT_EQ(tracker.GetStats(core::MemoryTag::UNKNOWN).used, 16u);
    EXPECT_STREQ(tracker.GetTagName(invalidTag), "Unknown");
}

} // namespace fbzz::tests
