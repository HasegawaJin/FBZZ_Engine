// FBZZ Engine
// MemorySystemTests.cpp | GoogleTest
// MemorySystem の所有するフレームアロケータとライフサイクルを自動検証する。
#include <gtest/gtest.h>

#include <Engine/Core/Memory/MemorySystem.hpp>

namespace fbzz::tests {

TEST(MemorySystemTest, InitializesAndShutsDownItsFrameAllocator)
{
    core::MemorySystem memory;

    ASSERT_TRUE(memory.Initialize(256));
    EXPECT_TRUE(memory.IsInitialized());
    memory.BeginFrame();
    EXPECT_NE(memory.GetFrameAllocator().Allocate(32), nullptr);
    memory.EndFrame();
    EXPECT_EQ(memory.GetFrameAllocator().GetStats().used, 0u);
    EXPECT_FALSE(memory.HasLeaks());

    memory.Shutdown();
    EXPECT_FALSE(memory.IsInitialized());
}

} // namespace fbzz::tests
