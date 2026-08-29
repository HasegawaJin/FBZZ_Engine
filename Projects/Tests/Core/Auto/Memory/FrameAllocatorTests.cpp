/// @file    FrameAllocatorTests.cpp
/// @brief   FrameAllocator のフレーム境界リセット契約を自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <gtest/gtest.h>

#include <Engine/Core/Memory/FrameAllocator.hpp>

namespace fbzz::tests {

TEST(FrameAllocatorTest, EndFrameResetsTemporaryAllocations)
{
    core::FrameAllocator allocator;
    ASSERT_TRUE(allocator.Initialize(256));

    allocator.BeginFrame();
    void* memory = allocator.Allocate(64);
    ASSERT_NE(memory, nullptr);
    EXPECT_TRUE(allocator.Owns(memory));
    EXPECT_GE(allocator.GetStats().used, 64u);

    allocator.EndFrame();
    EXPECT_EQ(allocator.GetStats().used, 0u);
    allocator.Shutdown();
}

TEST(FrameAllocatorTest, CanReuseCapacityAcrossFrames)
{
    core::FrameAllocator allocator;
    ASSERT_TRUE(allocator.Initialize(128));

    for (int frame = 0; frame < 3; ++frame) {
        allocator.BeginFrame();
        EXPECT_NE(allocator.Allocate(128), nullptr);
        allocator.EndFrame();
    }

    EXPECT_EQ(allocator.GetStats().used, 0u);
    allocator.Shutdown();
}

} // namespace fbzz::tests
