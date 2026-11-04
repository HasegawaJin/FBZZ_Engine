/// @file    TaskSystem.hpp
/// @brief   スレッドプールを中核としたバックグラウンド非同期タスク実行基盤。
/// @author  Hasegawa Jin
/// @date    2026-06-17

/// @note Submit(fn) -> future<T> の単一 API でどこからでも非同期タスクを投入できる。
#pragma once
#include <Engine/Api.hpp>
#include <functional>
#include <future>
#include <memory>
#include <cstdint>
#include <tuple>
#include <type_traits>

namespace fbzz {

class TaskSystem {
public:
    enum class ExternalWorkerCategory : std::uint8_t { Other, Audio, VolumeFluid };

    struct Stats {
        int workerCount = 0;
        int queuedCount = 0;
        int activeCount = 0;
        int externalWorkerCount = 0;
        int externalOtherWorkerCount = 0;
        int externalAudioWorkerCount = 0;
        int externalFluidWorkerCount = 0;
        uint64_t completedCount = 0;
        double totalQueueWaitMs = 0.0;
        double totalExecutionMs = 0.0;
        double maxQueueWaitMs = 0.0;
        double maxExecutionMs = 0.0;
    };

    /// @brief 共有プール以外で動くワーカーの実行期間を登録する。
    /// @note スコープの生成・破棄は対象ワーカー上で行う。
    class ExternalWorkerScope {
    public:
        FBZZ_ENGINE_API explicit ExternalWorkerScope(ExternalWorkerCategory category) noexcept;
        FBZZ_ENGINE_API ~ExternalWorkerScope();
        ExternalWorkerScope(const ExternalWorkerScope&) = delete;
        ExternalWorkerScope& operator=(const ExternalWorkerScope&) = delete;

    private:
        ExternalWorkerCategory m_category;
    };

    /// @brief エンジン起動時に一度だけ呼ぶ。numWorkers=0 で hw_concurrency を使用。
    FBZZ_ENGINE_API static void Init(int numWorkers = 0);
    FBZZ_ENGINE_API static void Shutdown();

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

    FBZZ_ENGINE_API static int WorkerCount();
    FBZZ_ENGINE_API static int PendingCount();
    /// @brief 共有プールと登録済み外部ワーカーの現在数、およびプールの累積待ち時間を返す。
    [[nodiscard]] FBZZ_ENGINE_API static Stats GetStats();

private:
    FBZZ_ENGINE_API static void SubmitRaw(std::function<void()> task);
};

} /// @note namespace fbzz
