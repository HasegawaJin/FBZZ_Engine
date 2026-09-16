/// @file    MemorySystemTests.cpp
/// @brief   MemorySystem の所有するフレームアロケータとライフサイクルを自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/Memory/MemorySystem.hpp>

#include <utility>

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

TEST_F(MemorySystemTest, ReportsNoLeakWhileNothingIsTracked)
{
    core::MemorySystem memory;
    ASSERT_TRUE(memory.Initialize(256));

    // Shutdown は «未解放が無いこと» を assert で確かめてから台帳を捨てる。
    // 何も追跡していない状態を漏れ扱いにすると、正常な終了で毎回止まる。
    EXPECT_EQ(memory.GetLeakCount(), 0u);
    EXPECT_FALSE(memory.HasLeaks());
}

TEST_F(MemorySystemTest, MoveConstructionTakesOverTheFrameAllocator)
{
    core::MemorySystem source;
    ASSERT_TRUE(source.Initialize(256));

    core::MemorySystem moved(std::move(source));

    EXPECT_TRUE(moved.IsInitialized());
    EXPECT_EQ(moved.GetFrameAllocator().GetStats().capacity, 256u);
    // 移動元が初期化済みのままだと、両方の Shutdown が同じ領域を解放しにいく。
    EXPECT_FALSE(source.IsInitialized());   // NOLINT(bugprone-use-after-move)
}

TEST_F(MemorySystemTest, MoveAssignmentReplacesTheSystemItAlreadyHeld)
{
    core::MemorySystem target;
    core::MemorySystem source;
    ASSERT_TRUE(target.Initialize(128));
    ASSERT_TRUE(source.Initialize(512));

    target = std::move(source);

    EXPECT_EQ(target.GetFrameAllocator().GetStats().capacity, 512u);
    EXPECT_FALSE(source.IsInitialized());   // NOLINT(bugprone-use-after-move)
}

TEST_F(MemorySystemTest, SelfMoveAssignmentKeepsTheSystemUsable)
{
    core::MemorySystem memory;
    ASSERT_TRUE(memory.Initialize(256));

    core::MemorySystem& alias = memory;
    memory = std::move(alias);

    EXPECT_TRUE(memory.IsInitialized());
    memory.BeginFrame();
    EXPECT_NE(memory.GetFrameAllocator().Allocate(32), nullptr);
    memory.EndFrame();
}

} // namespace fbzz::tests
