// FBZZ Engine
// TaskSystemConcurrencyTests.cpp | GoogleTest
// TaskSystem の複数タスク完了とワーカー数設定を自動検証する。
#include <gtest/gtest.h>

#include <Engine/Core/Concurrency/TaskSystem.hpp>

#include <atomic>
#include <future>
#include <vector>

namespace fbzz::tests {

TEST(TaskSystemConcurrencyTest, UsesTheRequestedWorkerCount)
{
    fbzz::TaskSystem::Init(1);
    EXPECT_EQ(fbzz::TaskSystem::WorkerCount(), 1);
    fbzz::TaskSystem::Shutdown();
}

TEST(TaskSystemConcurrencyTest, CompletesAllSubmittedTasks)
{
    fbzz::TaskSystem::Init(2);
    constexpr int TASK_COUNT = 8;
    std::atomic<int> completed{ 0 };
    std::vector<std::future<void>> futures;
    futures.reserve(TASK_COUNT);

    for (int i = 0; i < TASK_COUNT; ++i) {
        futures.push_back(fbzz::TaskSystem::Submit([&completed] {
            completed.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    for (auto& future : futures) {
        future.get();
    }

    EXPECT_EQ(completed.load(std::memory_order_relaxed), TASK_COUNT);
    fbzz::TaskSystem::Shutdown();
}

} // namespace fbzz::tests
