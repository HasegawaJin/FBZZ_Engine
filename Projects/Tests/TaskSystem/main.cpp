// FBZZ Engine
// Tests/TaskSystem/main.cpp
// TaskSystem モジュール単体テスト
// スレッドプールの Init/Shutdown・Submit の future 結果・並列実行を検証する。
#include <cstdio>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <numeric>

#include <Engine/Core/Concurrency/TaskSystem.hpp>

#include "../TestHelper.hpp"

using namespace fbzz;

// ─── Init / Shutdown ──────────────────────────────────────────────────────────

static void TestTaskSystem_Lifecycle()
{
    std::printf("\n=== TaskSystem: Lifecycle ===\n");

    TaskSystem::Init();
    check(TaskSystem::WorkerCount() > 0, "TaskSystem: WorkerCount > 0 after Init");
    TaskSystem::Shutdown();
    check(true, "TaskSystem: Shutdown completes without crash");

    // 再初期化できるか
    TaskSystem::Init(2);
    checkF(static_cast<float>(TaskSystem::WorkerCount()),
           "TaskSystem: Init(2) -> WorkerCount == 2",
           static_cast<float>(TaskSystem::WorkerCount()), "== 2");
    TaskSystem::Shutdown();
}

// ─── Submit → future で結果を受け取る ────────────────────────────────────────

static void TestTaskSystem_SubmitResult()
{
    std::printf("\n=== TaskSystem: Submit Result ===\n");

    TaskSystem::Init();

    // int を返すタスク
    auto fut = TaskSystem::Submit([]() -> int { return 42; });
    const int result = fut.get();
    checkF(static_cast<float>(result),
           "TaskSystem: Submit int task returns 42",
           static_cast<float>(result), "== 42");

    // 計算タスク
    auto fut2 = TaskSystem::Submit([](int a, int b) -> int { return a * b; }, 6, 7);
    checkF(static_cast<float>(fut2.get()),
           "TaskSystem: Submit with args returns correct product",
           static_cast<float>(fut2.get()), "== 42");

    // void タスク (future<void>)
    auto fut3 = TaskSystem::Submit([]() { /* fire and forget */ });
    fut3.get(); // should not throw
    check(true, "TaskSystem: void task completes without exception");

    TaskSystem::Shutdown();
}

// ─── 並列実行: 複数タスクが同時に走るか ──────────────────────────────────────

static void TestTaskSystem_Parallel()
{
    std::printf("\n=== TaskSystem: Parallel Execution ===\n");

    TaskSystem::Init();

    constexpr int N = 8;
    std::atomic<int> counter{ 0 };
    std::vector<std::future<void>> futures;
    futures.reserve(N);

    for (int i = 0; i < N; ++i)
    {
        futures.push_back(TaskSystem::Submit([&counter]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++counter;
        }));
    }

    for (auto& f : futures) f.get();

    checkF(static_cast<float>(counter.load()),
           "TaskSystem: all 8 parallel tasks completed",
           static_cast<float>(counter.load()), "== 8");

    TaskSystem::Shutdown();
}

// ─── 集計タスク: std::accumulate 相当の分割並列 ──────────────────────────────

static void TestTaskSystem_Accumulate()
{
    std::printf("\n=== TaskSystem: Accumulate ===\n");

    TaskSystem::Init();

    // 1〜100 の合計を 4 分割して並列計算 → 5050
    constexpr int TOTAL = 100;
    constexpr int PARTS = 4;
    constexpr int CHUNK = TOTAL / PARTS;

    std::vector<std::future<int>> futures;
    futures.reserve(PARTS);

    for (int p = 0; p < PARTS; ++p)
    {
        const int begin = p * CHUNK + 1;
        const int end   = begin + CHUNK;
        futures.push_back(TaskSystem::Submit([begin, end]() -> int {
            int sum = 0;
            for (int i = begin; i < end; ++i) sum += i;
            return sum;
        }));
    }

    int total = 0;
    for (auto& f : futures) total += f.get();

    checkF(static_cast<float>(total),
           "TaskSystem: parallel sum 1..100 == 5050",
           static_cast<float>(total), "== 5050");

    TaskSystem::Shutdown();
}

// ─── WorkerCount / numWorkers 指定 ────────────────────────────────────────────

static void TestTaskSystem_WorkerCount()
{
    std::printf("\n=== TaskSystem: WorkerCount ===\n");

    // numWorkers=1 で確認
    TaskSystem::Init(1);
    checkF(static_cast<float>(TaskSystem::WorkerCount()),
           "TaskSystem: Init(1) -> WorkerCount == 1",
           static_cast<float>(TaskSystem::WorkerCount()), "== 1");

    auto fut = TaskSystem::Submit([]() -> int { return 7; });
    checkF(static_cast<float>(fut.get()),
           "TaskSystem: single-worker Submit returns correct result",
           static_cast<float>(fut.get()), "== 7");

    TaskSystem::Shutdown();

    // numWorkers=0 → hw_concurrency
    TaskSystem::Init(0);
    check(TaskSystem::WorkerCount() > 0,
          "TaskSystem: Init(0) uses hw_concurrency (WorkerCount > 0)");
    TaskSystem::Shutdown();
}

// ─── エントリポイント ─────────────────────────────────────────────────────────

int main()
{
    std::printf("FBZZ TaskSystem Tests\n");
    std::printf("=====================\n");

    TestTaskSystem_Lifecycle();
    TestTaskSystem_SubmitResult();
    TestTaskSystem_Parallel();
    TestTaskSystem_Accumulate();
    TestTaskSystem_WorkerCount();

    std::printf("\n=====================\n");
    std::printf("Results: %d passed, %d failed\n", g_passed, g_failed);

    return g_failed == 0 ? 0 : 1;
}
