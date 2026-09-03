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

} // namespace

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
    // 二度目は «載っていない» ので false。
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

} // namespace fbzz::tests
