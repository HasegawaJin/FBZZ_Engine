// FBZZ Engine
// LinearAllocatorTests.cpp | GoogleTest
// LinearAllocator の一括再利用と容量契約を自動検証する。
#include <gtest/gtest.h>

#include <Engine/Core/Memory/LinearAllocator.hpp>

namespace fbzz::tests {

TEST(LinearAllocatorTest, InitializesAndAllocatesWithinCapacity)
{
    core::LinearAllocator allocator;
    ASSERT_TRUE(allocator.Initialize(128));
    EXPECT_TRUE(allocator.IsInitialized());

    void* first = allocator.Allocate(32);
    ASSERT_NE(first, nullptr);
    EXPECT_TRUE(allocator.Owns(first));
    EXPECT_GE(allocator.GetStats().used, 32u);
    EXPECT_EQ(allocator.GetStats().allocationCount, 1u);

    allocator.Shutdown();
    EXPECT_FALSE(allocator.IsInitialized());
}

TEST(LinearAllocatorTest, ResetReusesTheWholeRegion)
{
    core::LinearAllocator allocator;
    ASSERT_TRUE(allocator.Initialize(128));
    ASSERT_NE(allocator.Allocate(64), nullptr);

    allocator.Reset();

    EXPECT_EQ(allocator.GetStats().used, 0u);
    EXPECT_NE(allocator.Allocate(128), nullptr);
    allocator.Shutdown();
}

TEST(LinearAllocatorTest, RejectsAnAllocationThatExceedsCapacity)
{
    core::LinearAllocator allocator;
    ASSERT_TRUE(allocator.Initialize(128));

    EXPECT_EQ(allocator.Allocate(129), nullptr);
    allocator.Shutdown();
}

} // namespace fbzz::tests
