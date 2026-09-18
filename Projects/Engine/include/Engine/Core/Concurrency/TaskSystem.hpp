/// @file    TaskSystem.hpp
/// @brief   スレッドプールを中核としたバックグラウンド非同期タスク実行基盤。
/// @author  Hasegawa Jin
/// @date    2026-06-17
///
/// Submit(fn) -> future<T> の単一 API でどこからでも非同期タスクを投入できる。
#pragma once
#include <functional>
#include <future>
#include <memory>
#include <tuple>
#include <type_traits>

namespace fbzz {

class TaskSystem {
public:
    /// @brief エンジン起動時に一度だけ呼ぶ。numWorkers=0 で hw_concurrency を使用。
    static void Init(int numWorkers = 0);
    static void Shutdown();

    template<typename F, typename... Args>
    static auto Submit(F&& fn, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using R = std::invoke_result_t<F, Args...>;
        auto bound = [fn  = std::forward<F>(fn),
                      tup = std::make_tuple(std::forward<Args>(args)...)]() mutable -> R {
            return std::apply(std::move(fn), std::move(tup));
        };
        auto pt  = std::make_shared<std::packaged_task<R()>>(std::move(bound));
        auto fut = pt->get_future();
        SubmitRaw([pt = std::move(pt)]{ (*pt)(); });
        return fut;
    }

    static int WorkerCount();
    static int PendingCount();

private:
    static void SubmitRaw(std::function<void()> task);
};

} // namespace fbzz
