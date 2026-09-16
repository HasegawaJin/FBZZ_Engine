/// @file    TaskSystem.cpp
/// @brief   ThreadPool の実体。TaskSystem は Init() で生成した ThreadPool への static facade。
/// @author  Hasegawa Jin
/// @date    2026-06-17
#include "Engine/Core/Concurrency/TaskSystem.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace fbzz {

namespace {

class ThreadPool {
    using Task = std::function<void()>;

    std::deque<Task>         m_queue;
    std::vector<std::thread> m_workers;
    std::mutex               m_mutex;
    std::condition_variable  m_cv;
    std::atomic<bool>        m_stop{ false };
    std::atomic<int>         m_pending{ 0 };

    void WorkerLoop() {
        while (true) {
            Task task;
            {
                std::unique_lock lk(m_mutex);
                m_cv.wait(lk, [this]{ return m_stop.load() || !m_queue.empty(); });
                if (m_stop.load() && m_queue.empty()) return;
                task = std::move(m_queue.front());
                m_queue.pop_front();
            }
            task();
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
            m_queue.push_back(std::move(fn));
        }
        m_cv.notify_one();
    }

    int WorkerCount() const { return static_cast<int>(m_workers.size()); }
    int PendingCount() const { return m_pending.load(std::memory_order_relaxed); }
};

std::unique_ptr<ThreadPool> s_pool;

} // namespace

void TaskSystem::Init(int numWorkers) {
    const int n = (numWorkers > 0)
        ? numWorkers
        : static_cast<int>(std::thread::hardware_concurrency());
    s_pool = std::make_unique<ThreadPool>(n > 0 ? n : 1);
}

void TaskSystem::Shutdown() {
    s_pool.reset();
}

void TaskSystem::SubmitRaw(std::function<void()> task) {
    s_pool->Submit(std::move(task));
}

int TaskSystem::WorkerCount() { return s_pool ? s_pool->WorkerCount() : 0; }
int TaskSystem::PendingCount() { return s_pool ? s_pool->PendingCount() : 0; }

} // namespace fbzz
