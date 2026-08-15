// FBZZ Engine
// PoolAllocatorTests.cpp | GoogleTest
// PoolAllocator の固定ブロック再利用契約を自動検証する。
#include <gtest/gtest.h>

#include <Engine/Core/Memory/PoolAllocator.hpp>

namespace fbzz::tests {

TEST(PoolAllocatorTest, AllocatesExactlyTheConfiguredBlockCount)
{
    core::PoolAllocator allocator;
    ASSERT_TRUE(allocator.Initialize(32, 3));

    EXPECT_EQ(allocator.BlockSize(), 32u);
    EXPECT_EQ(allocator.BlockCount(), 3u);
    EXPECT_NE(allocator.AllocateBlock(), nullptr);
    EXPECT_NE(allocator.AllocateBlock(), nullptr);
    EXPECT_NE(allocator.AllocateBlock(), nullptr);
    EXPECT_EQ(allocator.AllocateBlock(), nullptr);

    allocator.Shutdown();
}

TEST(PoolAllocatorTest, FreeAndResetReturnBlocksToTheFreeList)
{
    core::PoolAllocator allocator;
    ASSERT_TRUE(allocator.Initialize(32, 2));

    void* first = allocator.AllocateBlock();
    ASSERT_NE(first, nullptr);
    allocator.Free(first);
    EXPECT_NE(allocator.AllocateBlock(), nullptr);

    allocator.Reset();
    EXPECT_NE(allocator.AllocateBlock(), nullptr);
    EXPECT_NE(allocator.AllocateBlock(), nullptr);
    EXPECT_EQ(allocator.AllocateBlock(), nullptr);

    allocator.Shutdown();
}

TEST(PoolAllocatorTest, RejectsARequestWithTheWrongBlockSize)
{
    core::PoolAllocator allocator;
    ASSERT_TRUE(allocator.Initialize(32, 1));

    EXPECT_EQ(allocator.Allocate(31), nullptr);
    EXPECT_NE(allocator.Allocate(32), nullptr);
    allocator.Shutdown();
}

} // namespace fbzz::tests
