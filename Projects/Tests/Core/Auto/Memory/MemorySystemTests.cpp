/// @file    MemorySystemTests.cpp
/// @brief   MemorySystem の所有するフレームアロケータとライフサイクルを自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/Memory/MemorySystem.hpp>

namespace fbzz::tests {

class MemorySystemTest : public testkit::EngineFixture {};

TEST_F(MemorySystemTest, InitializesAndShutsDownItsFrameAllocator)
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
