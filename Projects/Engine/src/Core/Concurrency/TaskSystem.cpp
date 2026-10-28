/// @file    TaskSystem.cpp
/// @brief   ThreadPool の実体。TaskSystem は Init() で生成した ThreadPool への static facade。
/// @author  Hasegawa Jin
/// @date    2026-06-17
#include "Engine/Core/Concurrency/TaskSystem.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace fbzz {

namespace {

class ThreadPool {
    using Task = std::function<void()>;

    struct QueuedTask {
        Task function;
        std::chrono::steady_clock::time_point enqueuedAt;
    };

    std::deque<QueuedTask>   m_queue;
    std::vector<std::thread> m_workers;
    std::mutex               m_mutex;
    std::condition_variable  m_cv;
    std::atomic<bool>        m_stop{ false };
    std::atomic<int>         m_pending{ 0 };
    std::atomic<int>         m_queued{ 0 };
    std::atomic<int>         m_active{ 0 };
    std::atomic<uint64_t>    m_completed{ 0 };
    std::atomic<uint64_t>    m_totalQueueWaitNs{ 0 };
    std::atomic<uint64_t>    m_totalExecutionNs{ 0 };
    std::atomic<uint64_t>    m_maxQueueWaitNs{ 0 };
    std::atomic<uint64_t>    m_maxExecutionNs{ 0 };

    static void UpdateMaximum(std::atomic<uint64_t>& maximum, uint64_t value)
    {
        uint64_t observed = maximum.load(std::memory_order_relaxed);
        while (observed < value
               && !maximum.compare_exchange_weak(observed, value, std::memory_order_relaxed)) {}
    }

    void WorkerLoop() {
        while (true) {
            QueuedTask queued;
            {
                std::unique_lock lk(m_mutex);
                m_cv.wait(lk, [this]{ return m_stop.load() || !m_queue.empty(); });
                if (m_stop.load() && m_queue.empty()) return;
                queued = std::move(m_queue.front());
                m_queue.pop_front();
                m_queued.fetch_sub(1, std::memory_order_relaxed);
                m_active.fetch_add(1, std::memory_order_relaxed);
            }
            const auto startedAt = std::chrono::steady_clock::now();
            queued.function();
            const auto finishedAt = std::chrono::steady_clock::now();
            const uint64_t queueWaitNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                startedAt - queued.enqueuedAt).count());
            const uint64_t executionNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                finishedAt - startedAt).count());
            m_totalQueueWaitNs.fetch_add(queueWaitNs, std::memory_order_relaxed);
            m_totalExecutionNs.fetch_add(executionNs, std::memory_order_relaxed);
            UpdateMaximum(m_maxQueueWaitNs, queueWaitNs);
            UpdateMaximum(m_maxExecutionNs, executionNs);
            m_completed.fetch_add(1, std::memory_order_relaxed);
            m_active.fetch_sub(1, std::memory_order_relaxed);
            m_pending.fetch_sub(1, std::memory_order_relaxed);
        }
    }

public:
    explicit ThreadPool(int n) {
        m_workers.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            m_workers.emplace_back([this]{ WorkerLoop(); });
    }

    ~ThreadPool() {
        {
            std::lock_guard lk(m_mutex);
            m_stop.store(true);
        }
        m_cv.notify_all();
        for (auto& w : m_workers) w.join();
    }

    void Submit(std::function<void()> fn) {
        m_pending.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lk(m_mutex);
            m_queue.push_back({ std::move(fn), std::chrono::steady_clock::now() });
            m_queued.fetch_add(1, std::memory_order_relaxed);
        }
        m_cv.notify_one();
    }

    int WorkerCount() const { return static_cast<int>(m_workers.size()); }
    int PendingCount() const { return m_pending.load(std::memory_order_relaxed); }

    TaskSystem::Stats GetStats() const
    {
        TaskSystem::Stats stats;
        stats.workerCount = WorkerCount();
        stats.queuedCount = m_queued.load(std::memory_order_relaxed);
        stats.activeCount = m_active.load(std::memory_order_relaxed);
        stats.completedCount = m_completed.load(std::memory_order_relaxed);
        constexpr double kNanosecondsToMilliseconds = 1.0 / 1'000'000.0;
        stats.totalQueueWaitMs = static_cast<double>(m_totalQueueWaitNs.load(std::memory_order_relaxed))
            * kNanosecondsToMilliseconds;
        stats.totalExecutionMs = static_cast<double>(m_totalExecutionNs.load(std::memory_order_relaxed))
            * kNanosecondsToMilliseconds;
        stats.maxQueueWaitMs = static_cast<double>(m_maxQueueWaitNs.load(std::memory_order_relaxed))
            * kNanosecondsToMilliseconds;
        stats.maxExecutionMs = static_cast<double>(m_maxExecutionNs.load(std::memory_order_relaxed))
            * kNanosecondsToMilliseconds;
        return stats;
    }
};

std::unique_ptr<ThreadPool> s_pool;
std::array<std::atomic<int>, 3> s_externalWorkers{};

} /// @note namespace

void TaskSystem::Init(int numWorkers) {
    const int n = (numWorkers > 0)
        ? numWorkers
        : static_cast<int>(std::thread::hardware_concurrency());
    s_pool = std::make_unique<ThreadPool>(n > 0 ? n : 1);
}

void TaskSystem::Shutdown() {
    s_pool.reset();
}

TaskSystem::ExternalWorkerScope::ExternalWorkerScope(ExternalWorkerCategory category) noexcept
    : m_category(category)
{
    s_externalWorkers[static_cast<size_t>(m_category)].fetch_add(1, std::memory_order_relaxed);
}

TaskSystem::ExternalWorkerScope::~ExternalWorkerScope()
{
    s_externalWorkers[static_cast<size_t>(m_category)].fetch_sub(1, std::memory_order_relaxed);
}

void TaskSystem::SubmitRaw(std::function<void()> task) {
    assert(s_pool && "TaskSystem::Init() has not been called");
    s_pool->Submit(std::move(task));
}

int TaskSystem::WorkerCount() { return s_pool ? s_pool->WorkerCount() : 0; }
int TaskSystem::PendingCount() { return s_pool ? s_pool->PendingCount() : 0; }

TaskSystem::Stats TaskSystem::GetStats()
{
    Stats stats = s_pool ? s_pool->GetStats() : Stats{};
    stats.externalOtherWorkerCount = s_externalWorkers[static_cast<size_t>(ExternalWorkerCategory::Other)]
        .load(std::memory_order_relaxed);
    stats.externalAudioWorkerCount = s_externalWorkers[static_cast<size_t>(ExternalWorkerCategory::Audio)]
        .load(std::memory_order_relaxed);
    stats.externalFluidWorkerCount = s_externalWorkers[static_cast<size_t>(ExternalWorkerCategory::VolumeFluid)]
        .load(std::memory_order_relaxed);
    stats.externalWorkerCount = stats.externalOtherWorkerCount + stats.externalAudioWorkerCount
        + stats.externalFluidWorkerCount;
    return stats;
}

} /// @note namespace fbzz
