/// @file    TaskSystemConcurrencyTests.cpp
/// @brief   TaskSystem の複数タスク完了とワーカー数設定を自動検証する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/Concurrency/TaskSystem.hpp>

#include <atomic>
#include <future>
#include <latch>
#include <vector>

namespace fbzz::tests {

class TaskSystemConcurrencyTest : public testkit::EngineFixture {};

TEST_F(TaskSystemConcurrencyTest, UsesTheRequestedWorkerCount)
{
    fbzz::TaskSystem::Init(1);
    EXPECT_EQ(fbzz::TaskSystem::WorkerCount(), 1);
    fbzz::TaskSystem::Shutdown();
}

TEST_F(TaskSystemConcurrencyTest, CompletesAllSubmittedTasks)
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

TEST_F(TaskSystemConcurrencyTest, AnswersZeroWithoutAPool)
{
    /// @note 問い合わせは Init の前にも来る (起動途中のシステムが並列度を見る)。落ちずに 0 と答えること。
    fbzz::TaskSystem::Shutdown();

    EXPECT_EQ(fbzz::TaskSystem::WorkerCount(), 0);
    EXPECT_EQ(fbzz::TaskSystem::PendingCount(), 0);
}

TEST_F(TaskSystemConcurrencyTest, HasNoPendingTasksRightAfterInit)
{
    fbzz::TaskSystem::Init(2);

    EXPECT_EQ(fbzz::TaskSystem::PendingCount(), 0);

    fbzz::TaskSystem::Shutdown();
}

TEST_F(TaskSystemConcurrencyTest, ReportsQueuedPoolWorkAndNamedExternalWorkers)
{
    fbzz::TaskSystem::Init(1);
    std::latch started(1);
    std::latch release(1);
    auto active = fbzz::TaskSystem::Submit([&started, &release] {
        started.count_down();
        release.wait();
    });
    started.wait();
    auto queued = fbzz::TaskSystem::Submit([] {});

    const auto poolStats = fbzz::TaskSystem::GetStats();
    EXPECT_EQ(poolStats.workerCount, 1);
    EXPECT_EQ(poolStats.activeCount, 1);
    EXPECT_EQ(poolStats.queuedCount, 1);
    EXPECT_EQ(poolStats.externalWorkerCount, 0);
    {
        fbzz::TaskSystem::ExternalWorkerScope worker(fbzz::TaskSystem::ExternalWorkerCategory::VolumeFluid);
        const auto externalStats = fbzz::TaskSystem::GetStats();
        EXPECT_EQ(externalStats.externalWorkerCount, 1);
        EXPECT_EQ(externalStats.externalFluidWorkerCount, 1);
    }
    release.count_down();
    auto accountingBarrier = fbzz::TaskSystem::Submit([] {});
    active.get();
    queued.get();
    accountingBarrier.get();
    const auto measuredStats = fbzz::TaskSystem::GetStats();
    EXPECT_GE(measuredStats.completedCount, 2u);
    EXPECT_GT(measuredStats.totalQueueWaitMs, 0.0);
    EXPECT_GT(measuredStats.totalExecutionMs, 0.0);
    fbzz::TaskSystem::Shutdown();
}

} /// @note namespace fbzz::tests
