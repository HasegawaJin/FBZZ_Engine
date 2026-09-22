/// @file    MemoryDebugTests.cpp
/// @brief   MemoryDebug の «登録したら抹消されるまで残る» 契約を自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/Memory/MemoryDebug.hpp>

namespace fbzz::tests {

class MemoryDebugTest : public testkit::EngineFixture {};

namespace {

core::AllocationInfo MakeInfo(void* pointer, core::MemoryTag tag, int line)
{
    core::AllocationInfo info;
    info.pointer = pointer;
    info.size = sizeof(int);
    info.alignment = alignof(int);
    info.tag = tag;
    info.allocatorName = "Test";
    info.file = "MemoryDebugTests.cpp";
    info.line = line;
    return info;
}

} /// @note namespace

TEST_F(MemoryDebugTest, TracksResourceUntilUntracked)
{
    core::MemoryDebug debug;
    int resource = 42;

    ASSERT_TRUE(debug.Track(MakeInfo(&resource, core::MemoryTag::RENDERER, 1)));
    ASSERT_EQ(debug.GetLiveCount(), 1u);
    ASSERT_NE(debug.GetLive(0), nullptr);
    EXPECT_EQ(debug.GetLive(0)->pointer, &resource);

    EXPECT_TRUE(debug.Untrack(&resource));
    EXPECT_EQ(debug.GetLiveCount(), 0u);
    /// @note 二度目は «載っていない» ので false。
    EXPECT_FALSE(debug.Untrack(&resource));
}

TEST_F(MemoryDebugTest, RejectsDuplicatePointer)
{
    core::MemoryDebug debug;
    int resource = 1;

    ASSERT_TRUE(debug.Track(MakeInfo(&resource, core::MemoryTag::CORE, 1)));
    EXPECT_FALSE(debug.Track(MakeInfo(&resource, core::MemoryTag::CORE, 2)));
    EXPECT_EQ(debug.GetLiveCount(), 1u);
}

TEST_F(MemoryDebugTest, CollectLiveReturnsEveryEntry)
{
    core::MemoryDebug debug;
    int first = 1;
    int second = 2;

    ASSERT_TRUE(debug.Track(MakeInfo(&first, core::MemoryTag::CORE, 1)));
    ASSERT_TRUE(debug.Track(MakeInfo(&second, core::MemoryTag::CORE, 2)));

    std::vector<core::AllocationInfo> live;
    debug.CollectLive(live);
    EXPECT_EQ(live.size(), 2u);
}

TEST_F(MemoryDebugTest, ResetRemovesAllTrackedResources)
{
    core::MemoryDebug debug;
    int first = 1;
    int second = 2;

    ASSERT_TRUE(debug.Track(MakeInfo(&first, core::MemoryTag::CORE, 1)));
    ASSERT_TRUE(debug.Track(MakeInfo(&second, core::MemoryTag::CORE, 2)));
    debug.Reset();

    EXPECT_EQ(debug.GetLiveCount(), 0u);
}

TEST_F(MemoryDebugTest, RejectsNullAndEnumeratesOnlyLiveSlots)
{
    core::MemoryDebug debug;
    int values[3]{};
    EXPECT_FALSE(debug.Track({}));
    EXPECT_FALSE(debug.Untrack(nullptr));
    for (auto& value : values) {
        ASSERT_TRUE(debug.Track(MakeInfo(&value, core::MemoryTag::CORE, 1)));
    }

    ASSERT_TRUE(debug.Untrack(&values[0]));

    EXPECT_EQ(debug.GetLiveBytes(), 2 * sizeof(int));
    ASSERT_NE(debug.GetLive(0), nullptr);
    ASSERT_NE(debug.GetLive(1), nullptr);
    EXPECT_EQ(debug.GetLive(0)->pointer, &values[1]);
    EXPECT_EQ(debug.GetLive(1)->pointer, &values[2]);
    EXPECT_EQ(debug.GetLive(2), nullptr);
}

TEST_F(MemoryDebugTest, RejectsOverflowWithoutCountingDroppedBytesAndReusesFreedSlots)
{
    core::MemoryDebug debug;
    std::vector<int> values(core::MemoryDebug::MAX_DEBUG_ALLOCATIONS + 1);
    for (std::size_t i = 0; i + 1 < values.size(); ++i) {
        ASSERT_TRUE(debug.Track(MakeInfo(&values[i], core::MemoryTag::CORE, 1)));
    }

    EXPECT_FALSE(debug.Track(MakeInfo(&values.back(), core::MemoryTag::CORE, 2)));

    EXPECT_EQ(debug.GetDroppedCount(), 1u);
    EXPECT_EQ(debug.GetLiveCount(), values.size() - 1);
    EXPECT_EQ(debug.GetLiveBytes(), (values.size() - 1) * sizeof(int));
    ASSERT_TRUE(debug.Untrack(&values.front()));
    ASSERT_TRUE(debug.Track(MakeInfo(&values.back(), core::MemoryTag::CORE, 3)));
    EXPECT_EQ(debug.GetLiveCount(), values.size() - 1);
    debug.Reset();
    EXPECT_EQ(debug.GetLiveCount(), 0u);
    EXPECT_EQ(debug.GetLiveBytes(), 0u);
    EXPECT_EQ(debug.GetDroppedCount(), 0u);
    ASSERT_TRUE(debug.Track(MakeInfo(&values.front(), core::MemoryTag::CORE, 4)));
    ASSERT_NE(debug.GetLive(0), nullptr);
    EXPECT_EQ(debug.GetLive(0)->allocationId, 1u);
}

} /// @note namespace fbzz::tests
