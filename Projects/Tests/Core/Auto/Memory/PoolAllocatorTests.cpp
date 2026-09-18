/// @file    PoolAllocatorTests.cpp
/// @brief   PoolAllocator の固定ブロック再利用契約を自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/Memory/PoolAllocator.hpp>

namespace fbzz::tests {

class PoolAllocatorTest : public testkit::EngineFixture {};

TEST_F(PoolAllocatorTest, AllocatesExactlyTheConfiguredBlockCount)
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

TEST_F(PoolAllocatorTest, FreeAndResetReturnBlocksToTheFreeList)
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

/// @note 基底 Allocator の契約は «容量不足なら nullptr» のみでサイズの完全一致は求めない。CreateObject<T> は Allocate(sizeof(T), alignof(T)) を呼ぶため、完全一致を要求すると sizeof(T)==blockSize のときしか使えなくなる。
/// @note blockSize を超える要求は Allocate 内の assert が先に止める。Development 構成は NDEBUG を定義しない (アサート有効) ためその経路はテストから踏めない。
TEST_F(PoolAllocatorTest, AcceptsAnyRequestThatFitsInTheBlock)
{
    core::PoolAllocator allocator;
    ASSERT_TRUE(allocator.Initialize(32, 2));

    EXPECT_NE(allocator.Allocate(31), nullptr);
    EXPECT_NE(allocator.Allocate(32), nullptr);
    /// @note 使い切れば容量不足で nullptr
    EXPECT_EQ(allocator.Allocate(32), nullptr);

    allocator.Shutdown();
}

} // namespace fbzz::tests
