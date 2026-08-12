// FBZZ Engine
// StackAllocatorTests.cpp | GoogleTest
// StackAllocator の LIFO 解放契約を自動検証する。
#include <gtest/gtest.h>

#include <Engine/Core/Memory/StackAllocator.hpp>

namespace fbzz::tests {

TEST(StackAllocatorTest, FreesOnlyTheMostRecentAllocation)
{
    core::StackAllocator allocator;
    ASSERT_TRUE(allocator.Initialize(256));

    void* first = allocator.Allocate(32);
    void* second = allocator.Allocate(32);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(allocator.GetStats().activeCount, 2u);

    allocator.Free(second);
    EXPECT_EQ(allocator.GetStats().activeCount, 1u);

    void* reused = allocator.Allocate(32);
    EXPECT_NE(reused, nullptr);
    EXPECT_EQ(allocator.GetStats().activeCount, 2u);

    allocator.Reset();
    EXPECT_EQ(allocator.GetStats().used, 0u);
    allocator.Shutdown();
}

TEST(StackAllocatorTest, RejectsAnAllocationThatExceedsCapacity)
{
    core::StackAllocator allocator;
    ASSERT_TRUE(allocator.Initialize(64));

    EXPECT_EQ(allocator.Allocate(65), nullptr);
    allocator.Shutdown();
}

} // namespace fbzz::tests
