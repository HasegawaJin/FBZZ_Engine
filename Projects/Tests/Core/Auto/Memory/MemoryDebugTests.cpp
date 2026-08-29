/// @file    MemoryDebugTests.cpp
/// @brief   MemoryDebug の非所有 weak_ptr 追跡契約を自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <gtest/gtest.h>

#include <Engine/Core/Memory/MemoryDebug.hpp>

#include <memory>

namespace fbzz::tests {

TEST(MemoryDebugTest, TracksLiveSharedResourcesWithoutExtendingLifetime)
{
    core::MemoryDebug debug;
    auto resource = std::make_shared<int>(42);

    ASSERT_TRUE(debug.TrackShared(resource, core::MemoryTag::RENDERER,
                                  "Test", "MemoryDebugTests.cpp", 1));
    ASSERT_EQ(debug.GetLiveCount(), 1u);
    ASSERT_NE(debug.GetLive(0), nullptr);
    EXPECT_EQ(debug.GetLive(0)->pointer, resource.get());

    resource.reset();
    debug.SweepExpired();
    EXPECT_EQ(debug.GetLiveCount(), 0u);
}

TEST(MemoryDebugTest, ResetRemovesAllTrackedResources)
{
    core::MemoryDebug debug;
    auto first = std::make_shared<int>(1);
    auto second = std::make_shared<int>(2);

    ASSERT_TRUE(debug.TrackShared(first, core::MemoryTag::CORE, "Test", "file", 1));
    ASSERT_TRUE(debug.TrackShared(second, core::MemoryTag::CORE, "Test", "file", 2));
    debug.Reset();

    EXPECT_EQ(debug.GetLiveCount(), 0u);
}

} // namespace fbzz::tests
