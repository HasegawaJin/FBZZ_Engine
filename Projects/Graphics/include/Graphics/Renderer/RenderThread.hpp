/// @file    RenderThread.hpp
/// @brief   所有済み描画入力を最大 1 フレームだけ処理する専用 CPU スレッド。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <Core/Profiler/FrameTimeline.hpp>

namespace fbzz::renderer {

/// @note 入力はフレーム自身が所有し、Scene/Component や呼び出し元の一時領域を参照しない。
/// @note 受付済みフレームの Execute と破棄は同じ Render Thread で行う。拒否された入力は producer が破棄する。
/// @see Docs/design/render-thread.md
class IRenderFrame {
public:
    virtual ~IRenderFrame() = default;
    virtual void Execute() = 0;
};

/// @note Submit/Wait は producer の同期点。ゲーム更新は Submit 間で処理中のフレームと並行できる。
/// @note Stop は新規入力を閉じ、受付済み入力の実行と破棄を待ってから thread を join する。
/// @note この executor は ImGui/Scene/RHI を自動的に thread-safe にしない。入力の所有契約が前提。
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/threaded-rendering-in-unreal-engine Thread specific data structures.
class RenderThread final {
public:
    /// @brief A move-only reservation owns the existing idle-slot mutex without publishing input.
    /// @note Dropping an unpublished reservation leaves serial, frame and busy state unchanged.
    class SubmissionReservation final {
    public:
        SubmissionReservation() = default;
        SubmissionReservation(SubmissionReservation&&) noexcept = default;
        SubmissionReservation& operator=(SubmissionReservation&&) noexcept = default;
        SubmissionReservation(const SubmissionReservation&) = delete;
        SubmissionReservation& operator=(const SubmissionReservation&) = delete;
        [[nodiscard]] bool IsValid() const { return m_owner && m_lock.owns_lock(); }
        /// @pre This is a valid reservation, its executor remains alive, and publication stays on the reserving thread.
        /// @pre Frame owns all worker inputs.
        /// @note Publishes before notification; no wait, callback, or producer-side execution occurs.
        [[nodiscard]] bool Publish(std::unique_ptr<IRenderFrame> frame);
    private:
        friend class RenderThread;
        SubmissionReservation(RenderThread& owner, uint64_t serial, std::unique_lock<std::mutex> lock)
            : m_owner(&owner), m_serial(serial), m_lock(std::move(lock)) {}
        RenderThread* m_owner = nullptr;
        uint64_t m_serial = 0;
        std::unique_lock<std::mutex> m_lock;
    };

    /// @return Invalid if the mutex/slot is busy, stopping, worker-owned, or serial is not increasing.
    /// @pre Caller holds no reservation from this executor.
    /// @note Uses try-lock only; input construction and serial commitment occur after success.
    [[nodiscard]] SubmissionReservation TryReserveSubmission(uint64_t serial);

    RenderThread();
    ~RenderThread();
    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    /// @return 受付済みなら true。停止中、空入力、単調増加しない serial では false。
    /// @note 前フレームの入力が生存する間だけ待つ。serial は application frame の識別値。
    /// @pre Render Thread 自身から呼ばないこと。
    [[nodiscard]] bool Submit(uint64_t serial, std::unique_ptr<IRenderFrame> frame);

    /// @return 指定 serial 以上の CPU 処理と入力破棄が完了したら true。未受付の未来 serial なら false。
    /// @pre Render Thread 自身から呼ばないこと。
    [[nodiscard]] bool WaitUntilCompleted(uint64_t serial);

    /// @note 受付済みの最新入力が破棄されるまで待つ。GPU の fence は別途待つ。
    /// @pre Render Thread 自身から呼ばないこと。
    void WaitIdle();

    /// @return True only after accepting the mutex with no queued or executing input, including its destruction.
    /// @pre Caller holds no reservation from this executor.
    /// @note Worker calls and a busy mutex return false without waiting or pumping; true is an observation, not a reservation.
    [[nodiscard]] bool TryIsIdle();

    /// @pre producer から呼ぶ。Stop 同士を並行実行せず、Render Thread 自身から呼ばないこと。
    void Stop();

    /// @note 同期呼び出しの再入を判定し、Render Thread が自分の完了を待たないための境界。
    [[nodiscard]] bool IsCurrentThread() const { return CalledFromWorker(); }
    /// @note 起動時に producer が設定する。Win32 の送信メッセージを完了待ち中にも処理するための hook。
    void SetWaitPump(void (*pump)()) { m_waitPump = pump; }

private:
    void Run();
    [[nodiscard]] bool CalledFromWorker() const;
    void Wait(std::unique_lock<std::mutex>& lock, const std::function<bool()>& ready);

    std::mutex m_mutex;
    std::condition_variable m_wakeup;
    std::condition_variable m_completed;
    std::unique_ptr<IRenderFrame> m_frame;
    profiler::frame_timeline::Context m_frameTimeline;
    uint64_t m_submittedSerial = 0;
    uint64_t m_completedSerial = 0;
    bool m_busy = false;
    bool m_stopping = false;
    void (*m_waitPump)() = nullptr;
    std::jthread m_thread;
    const std::thread::id m_workerId;
};

} /// @note namespace fbzz::renderer
