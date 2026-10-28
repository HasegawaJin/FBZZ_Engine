/// @file    RenderThread.cpp
/// @brief   有界フレーム入力の受付、実行、破棄と終了時の完了待ち。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Graphics/Renderer/RenderThread.hpp>
#include <Graphics/Renderer/RenderCpuTrace.hpp>
#include <Core/ExternalCallbackBoundary.hpp>
#include <Core/Profiler/FrameTimeline.hpp>
#include <cassert>
#include <chrono>
#include <utility>

namespace fbzz::renderer {

RenderThread::RenderThread() : m_thread([this] { Run(); }), m_workerId(m_thread.get_id()) { cpu_trace::RegisterThread(this); }

RenderThread::~RenderThread()
{
    Stop();
    cpu_trace::UnregisterThread(this);
}

bool RenderThread::CalledFromWorker() const
{
    return std::this_thread::get_id() == m_workerId;
}

void RenderThread::Wait(std::unique_lock<std::mutex>& lock, const std::function<bool()>& ready)
{
    /// @note Predicates read only mutex-protected completion state; the diagnostic probe never changes readiness.
    const cpu_trace::WaitScope traceWait(cpu_trace::HasSelectedFrame() && !ready());
    if (!m_waitPump) { m_completed.wait(lock, ready); return; }
    while (!ready()) {
        if (m_completed.wait_for(lock, std::chrono::milliseconds(1), ready)) break;
        lock.unlock();
        {
            const cpu_trace::PumpScope tracePump;
            const profiler::frame_timeline::Scope timelinePump(profiler::frame_timeline::Event::WAIT_PUMP);
            (void)core::NotifyBeforeExternalCallback();
            m_waitPump();
        }
        lock.lock();
    }
}

bool RenderThread::Submit(uint64_t serial, std::unique_ptr<IRenderFrame> frame)
{
    assert(!CalledFromWorker());
    if (CalledFromWorker() || !frame || serial == 0) return false;
    cpu_trace::AcquireScope traceAcquire(cpu_trace::AcquireKind::THREAD_MUTEX);
    const auto timelineMutex = profiler::frame_timeline::Begin(profiler::frame_timeline::Event::THREAD_MUTEX);
    std::unique_lock lock(m_mutex);
    profiler::frame_timeline::End(timelineMutex);
    traceAcquire.Acquired();
    Wait(lock, [this] { return !m_busy || m_stopping; });
    if (m_stopping || serial <= m_submittedSerial) return false;
    m_submittedSerial = serial;
    m_frameTimeline = profiler::frame_timeline::CurrentContext();
    m_frameTimeline.submissionSerial = serial;
    profiler::frame_timeline::Mark(profiler::frame_timeline::Event::PACKET_ACCEPT, m_frameTimeline);
    m_frame = std::move(frame);
    m_busy = true;
    lock.unlock();
    m_wakeup.notify_one();
    return true;
}

RenderThread::SubmissionReservation RenderThread::TryReserveSubmission(uint64_t serial)
{
    if (CalledFromWorker() || serial == 0) return {};
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock() || m_busy || m_stopping || serial <= m_submittedSerial) return {};
    return SubmissionReservation(*this, serial, std::move(lock));
}

bool RenderThread::SubmissionReservation::Publish(std::unique_ptr<IRenderFrame> frame)
{
    if (!IsValid() || !frame) return false;
    auto* owner = m_owner;
    owner->m_submittedSerial = m_serial;
    owner->m_frameTimeline = profiler::frame_timeline::CurrentContext();
    owner->m_frameTimeline.submissionSerial = m_serial;
    profiler::frame_timeline::Mark(profiler::frame_timeline::Event::PACKET_ACCEPT, owner->m_frameTimeline);
    owner->m_frame = std::move(frame);
    owner->m_busy = true;
    m_owner = nullptr;
    m_lock.unlock();
    owner->m_wakeup.notify_one();
    return true;
}

bool RenderThread::WaitUntilCompleted(uint64_t serial)
{
    assert(!CalledFromWorker());
    if (CalledFromWorker() || serial == 0) return false;
    cpu_trace::AcquireScope traceAcquire(cpu_trace::AcquireKind::THREAD_MUTEX);
    const auto timelineMutex = profiler::frame_timeline::Begin(profiler::frame_timeline::Event::THREAD_MUTEX);
    std::unique_lock lock(m_mutex);
    profiler::frame_timeline::End(timelineMutex);
    traceAcquire.Acquired();
    if (serial > m_submittedSerial) return false;
    Wait(lock, [this, serial] { return m_completedSerial >= serial; });
    return true;
}

bool RenderThread::TryIsIdle()
{
    if (CalledFromWorker()) return false;
    std::unique_lock lock(m_mutex, std::try_to_lock);
    return lock.owns_lock() && !m_busy && !m_frame;
}

void RenderThread::WaitIdle()
{
    assert(!CalledFromWorker());
    if (CalledFromWorker()) return;
    cpu_trace::AcquireScope traceAcquire(cpu_trace::AcquireKind::THREAD_MUTEX);
    const auto timelineMutex = profiler::frame_timeline::Begin(profiler::frame_timeline::Event::THREAD_MUTEX);
    std::unique_lock lock(m_mutex);
    profiler::frame_timeline::End(timelineMutex);
    traceAcquire.Acquired();
    Wait(lock, [this] { return !m_busy; });
}

void RenderThread::Stop()
{
    assert(!CalledFromWorker());
    if (CalledFromWorker()) return;
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_wakeup.notify_one();
    m_completed.notify_all();
    WaitIdle();
    if (m_thread.joinable()) m_thread.join();
    cpu_trace::ThreadStopped(this);
}

void RenderThread::Run()
{
    for (;;) {
        std::unique_ptr<IRenderFrame> frame;
        uint64_t serial = 0;
        profiler::frame_timeline::Context timelineContext;
        {
            std::unique_lock lock(m_mutex);
            m_wakeup.wait(lock, [this] { return m_frame != nullptr || m_stopping; });
            if (!m_frame) return;
            frame = std::move(m_frame);
            serial = m_submittedSerial;
            timelineContext = m_frameTimeline;
        }
        cpu_trace::PacketScope tracePacket(this, serial);
        const profiler::frame_timeline::ContextScope timelineOrigin(timelineContext);
        const profiler::frame_timeline::Scope timelinePacket(profiler::frame_timeline::Event::PACKET);
        {
            const profiler::frame_timeline::Scope timelineExecute(profiler::frame_timeline::Event::PACKET_EXECUTE);
            frame->Execute();
        }
        tracePacket.BeginDestruction();
        /// @note 完了通知より先に破棄し、producer が次の入力や資源寿命へ進んでも参照を残さない。
        {
            const profiler::frame_timeline::Scope timelineDestroy(profiler::frame_timeline::Event::PACKET_DESTROY);
            frame.reset();
        }
        tracePacket.FinishedDestruction();
        {
            std::lock_guard lock(m_mutex);
            m_completedSerial = serial;
            m_busy = false;
        }
        m_completed.notify_all();
    }
}

} /// @note namespace fbzz::renderer
