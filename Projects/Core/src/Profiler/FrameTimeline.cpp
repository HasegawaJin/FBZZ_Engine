/// @file    FrameTimeline.cpp
/// @brief   QPC のイベント収集、フレーム境界、停止後の出力制御。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#include <Core/Profiler/FrameTimeline.hpp>
#include "FrameTimelineCapture.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <realtimeapiset.h>
#endif

namespace fbzz::profiler::frame_timeline {
namespace {

using detail::Capture;
using detail::CpuCounters;
using detail::Edge;
using detail::EventRow;
using detail::FrameRow;
using detail::INVALID_INDEX;
using detail::CAPTURE_ANCHOR_COUNT;
using detail::DEFERRED_SLOT_BITS;
using detail::MAX_PENDING_FINE_SPANS;
using detail::FineThresholdTicks;
using detail::IsFineSpan;

constexpr size_t MAX_FRAMES = 60000;
constexpr size_t DEFAULT_EVENTS = 2000000;
constexpr size_t MAX_EVENTS = 4000000;
constexpr uint64_t DEFERRED_TICKET_BIT = uint64_t{1} << 63;
struct TimelineState {
    std::mutex coldMutex;
    std::unique_ptr<Capture> capture;
    std::string output;
    std::atomic<bool> enabled{false};
    size_t eventCapacity = DEFAULT_EVENTS;
    bool initialized = false;
};
TimelineState& State() { static TimelineState state; return state; }
thread_local Context s_context{};
thread_local uint64_t s_mainFrameTicket = 0, s_mainFrameIndex = INVALID_INDEX;
struct PendingFineSpan {
    Capture* capture = nullptr;
    Context context{};
    uint64_t ticket = 0, qpc = 0, value = 0;
    Event event = Event::MAIN_FRAME;
};
thread_local std::array<PendingFineSpan, MAX_PENDING_FINE_SPANS> s_pendingFineSpans{};

uint32_t OsThreadId()
{
#if defined(_WIN32)
    return GetCurrentThreadId();
#else
    return 0;
#endif
}

/// @note Raw QPC plus frequency and existing profile milliseconds permit explicit clock-domain calibration.
/// @see https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps QueryPerformanceCounter.
uint64_t ReadQpc(Capture& capture)
{
    capture.wallQueries.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
    LARGE_INTEGER value{};
    if (QueryPerformanceCounter(&value)) return static_cast<uint64_t>(value.QuadPart);
#endif
    capture.invalidBoundaries.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

/// @note FILETIME counts thread execution in 100 ns units and has limited per-frame precision.
/// @see https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadtimes GetThreadTimes.
/// @see https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime Raw cycles must not be converted into elapsed time.
uint64_t RecordThreadCpuCounters(Capture& capture)
{
    const uint64_t index = detail::ReserveBounded(capture.cpuCount, capture.cpuCapacity, capture.droppedCpu);
    if (index == INVALID_INDEX) return INVALID_INDEX;
    capture.cpuQueries.fetch_add(1, std::memory_order_relaxed);
    auto& row = capture.cpu[index];
#if defined(_WIN32)
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) {
        row.kernel = (static_cast<uint64_t>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
        row.user = (static_cast<uint64_t>(user.dwHighDateTime) << 32) | user.dwLowDateTime;
        row.cpuAvailable = true;
    }
    ULONG64 cycles = 0;
    if (QueryThreadCycleTime(GetCurrentThread(), &cycles)) { row.cycles = cycles; row.cyclesAvailable = true; }
#endif
    return index;
}

bool IsInCaptureWindow(const Capture& capture, const Context& context)
{
    const uint64_t first = capture.warmup == 0 ? 1 : capture.warmup;
    return context.captureFrameOrdinal >= first && context.captureFrameOrdinal <= capture.warmup + capture.samples;
}

class CaptureWriter final {
public:
    explicit CaptureWriter(Capture& capture) : m_capture(capture)
    {
        m_capture.activeWriters.fetch_add(1, std::memory_order_acquire);
        m_valid = !m_capture.closing.load(std::memory_order_acquire);
    }
    ~CaptureWriter() { m_capture.activeWriters.fetch_sub(1, std::memory_order_release); }
    [[nodiscard]] bool Valid() const { return m_valid; }
private:
    Capture& m_capture;
    bool m_valid = false;
};

uint64_t AppendEvent(Capture& capture, Event event, Edge edge, Context context, uint64_t scope,
    uint64_t value, bool threadCpu, uint64_t qpc)
{
    const uint64_t index = detail::ReserveBounded(capture.eventCount, capture.eventCapacity, capture.droppedEvents);
    if (index == INVALID_INDEX) return 0;
    auto& row = capture.events[index];
    row.context = context;
    row.qpc = qpc;
    row.scope = scope;
    row.value = value;
    row.osThreadId = OsThreadId();
    row.event = event;
    row.edge = edge;
    row.threadCpu = threadCpu;
    if (threadCpu) {
        row.cpu = RecordThreadCpuCounters(capture);
    }
    return index + 1;
}

uint64_t BeginAt(Capture& capture, Event event, Context context, uint64_t value, bool threadCpu, uint64_t qpc)
{
    const uint64_t ticket = AppendEvent(capture, event, Edge::BEGIN, context, 0, value, threadCpu, qpc);
    if (ticket != 0) {
        capture.events[ticket - 1].scope = ticket;
        capture.activeScopes.fetch_add(1, std::memory_order_relaxed);
    }
    return ticket;
}

void EndAt(Capture& capture, uint64_t ticket, uint64_t value, uint64_t qpc)
{
    if (ticket == 0 || ticket > capture.eventCapacity) return;

    auto& begin = capture.events[ticket - 1];
    if (begin.edge != Edge::BEGIN || begin.osThreadId != OsThreadId() || begin.ended.exchange(true)) {
        capture.invalidBoundaries.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    (void)AppendEvent(capture, begin.event, Edge::END, begin.context, ticket, value, begin.threadCpu, qpc);
    capture.activeScopes.fetch_sub(1, std::memory_order_relaxed);
}

/// @note Fixed TLS slots preserve start timestamps without publishing two rows for subthreshold spans; exhaustion uses the original unfiltered path.
uint64_t DeferFineSpan(Capture& capture, Event event, Context context, uint64_t value, uint64_t qpc)
{
    for (size_t index = 0; index < s_pendingFineSpans.size(); ++index) {
        auto& pending = s_pendingFineSpans[index];
        if (pending.ticket != 0) continue;
        const uint64_t sequence = (capture.fineTicketSequence.fetch_add(1, std::memory_order_relaxed) + 1)
            & ((DEFERRED_TICKET_BIT - 1) >> DEFERRED_SLOT_BITS);
        const uint64_t ticket = DEFERRED_TICKET_BIT | (sequence << DEFERRED_SLOT_BITS) | index;
        pending = {&capture, context, ticket, qpc, value, event};
        capture.activeScopes.fetch_add(1, std::memory_order_relaxed);
        return ticket;
    }
    capture.fineSpanFallbacks.fetch_add(1, std::memory_order_relaxed);
    return BeginAt(capture, event, context, value, false, qpc);
}

/// @note Retained spans publish Begin then End at their original QPC values; publication order need not be timestamp order.
void EndFineSpan(Capture& capture, uint64_t ticket, uint64_t value, uint64_t qpc)
{
    auto& slot = s_pendingFineSpans[ticket & (MAX_PENDING_FINE_SPANS - 1)];
    if (slot.ticket != ticket || slot.capture != &capture) {
        capture.invalidBoundaries.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto pending = slot;
    slot.ticket = 0;
    if (qpc >= pending.qpc && qpc - pending.qpc < FineThresholdTicks(capture)) {
        const auto index = static_cast<size_t>(pending.event);
        capture.filteredFineScopes[index].fetch_add(1, std::memory_order_relaxed);
        capture.filteredFineTicks[index].fetch_add(qpc - pending.qpc, std::memory_order_relaxed);
    } else {
        const uint64_t published = BeginAt(capture, pending.event, pending.context, pending.value, false, pending.qpc);
        EndAt(capture, published, value, qpc);
    }
    capture.activeScopes.fetch_sub(1, std::memory_order_relaxed);
}

size_t EventLimitFromEnvironment()
{
    const char* text = std::getenv("FBZZ_FRAME_TIMELINE_MAX_EVENTS");
    if (!text || !*text) return DEFAULT_EVENTS;
    size_t value = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || value > MAX_EVENTS / 10) return DEFAULT_EVENTS;
        value = value * 10 + static_cast<size_t>(*p - '0');
        if (value > MAX_EVENTS) return DEFAULT_EVENTS;
    }
    return value >= 1024 ? value : DEFAULT_EVENTS;
}

} /// @note namespace

void InitializeFromEnvironment()
{
    auto& state = State();
    std::lock_guard lock(state.coldMutex);
    if (state.initialized) return;
    state.initialized = true;
    const char* output = std::getenv("FBZZ_FRAME_TIMELINE_OUTPUT");
    if (output && *output) state.output = output;
    state.eventCapacity = EventLimitFromEnvironment();
}

bool IsEnabled() { return State().enabled.load(std::memory_order_acquire); }

bool ArmFromEnvironment(uint64_t observerSession, size_t warmupFrames, size_t sampleFrames)
{
    InitializeFromEnvironment();
    auto& state = State();
    std::lock_guard lock(state.coldMutex);
    if (state.output.empty() || state.capture || observerSession == 0 || sampleFrames == 0
        || sampleFrames > MAX_FRAMES || warmupFrames > MAX_FRAMES) return false;
    auto capture = std::make_unique<Capture>(observerSession, warmupFrames, sampleFrames, state.eventCapacity);
#if defined(_WIN32)
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return false;
    capture->frequency = static_cast<uint64_t>(frequency.QuadPart);
#else
    return false;
#endif
    state.capture = std::move(capture);
    state.enabled.store(true, std::memory_order_release);
    return true;
}

void CancelCapture(uint64_t observerSession)
{
    if (!IsEnabled()) return;
    auto& capture = *State().capture;
    if (capture.observerSession == observerSession) capture.cancelled.store(true, std::memory_order_release);
}

Context CurrentContext() { return IsEnabled() ? s_context : Context{}; }

ContextScope::ContextScope(Context context)
{
    if (!IsEnabled()) return;

    m_previous = s_context;
    s_context = context;
    m_active = true;
}

ContextScope::~ContextScope()
{
    if (m_active) {
        s_context = m_previous;
    }
}

uint64_t Begin(Event event, Context context, uint64_t value, bool threadCpu)
{
    if (!IsEnabled()) return 0;
    auto& capture = *State().capture;
    if (context.captureFrameOrdinal == 0) context = s_context;
    if (!IsInCaptureWindow(capture, context) || capture.cancelled.load(std::memory_order_acquire)) return 0;
    const CaptureWriter writer(capture);
    if (!writer.Valid()) return 0;
    const uint64_t now = ReadQpc(capture);
    return !threadCpu && IsFineSpan(event) ? DeferFineSpan(capture, event, context, value, now)
        : BeginAt(capture, event, context, value, threadCpu, now);
}

void End(uint64_t ticket, uint64_t value)
{
    if (ticket == 0 || !IsEnabled()) return;
    auto& capture = *State().capture;
    const CaptureWriter writer(capture);
    if (writer.Valid()) {
        const uint64_t now = ReadQpc(capture);
        if ((ticket & DEFERRED_TICKET_BIT) != 0) EndFineSpan(capture, ticket, value, now);
        else EndAt(capture, ticket, value, now);
    }
}

void Mark(Event event, Context context, uint64_t value)
{
    if (!IsEnabled()) return;
    auto& capture = *State().capture;
    if (context.captureFrameOrdinal == 0) context = s_context;
    if (!IsInCaptureWindow(capture, context) || capture.cancelled.load(std::memory_order_acquire)) return;
    const CaptureWriter writer(capture);
    if (writer.Valid()) (void)AppendEvent(capture, event, Edge::INSTANT, context, 0, value, false, ReadQpc(capture));
}

Scope::Scope(Event event, Context context, uint64_t value, bool threadCpu) : m_ticket(Begin(event, context, value, threadCpu)) {}
Scope::~Scope() { End(m_ticket); }

void BeginApplicationFrame(uint64_t applicationFrameSerial, uint64_t resourceFrameSerial, double profileStartMs)
{
    if (!IsEnabled()) return;
    auto& capture = *State().capture;
    if (capture.cancelled.load(std::memory_order_acquire)) return;
    const auto role = capture.window.Next();
    s_context = {capture.window.Seen(), applicationFrameSerial, resourceFrameSerial};
    if (role == detail::FrameRole::UNSELECTED || role == detail::FrameRole::FINISHED) {
        s_mainFrameIndex = INVALID_INDEX;
        return;
    }

    const uint64_t now = ReadQpc(capture);
    if (s_mainFrameTicket != 0) {
        EndAt(capture, s_mainFrameTicket, 0, now);
        s_mainFrameTicket = 0;
        capture.invalidBoundaries.fetch_add(1);
    }

    /// @note This Begin closes the preceding frame's forward interval, even if that frame missed End.
    if (capture.frameCount > 0) {
        auto& previous = capture.frames[capture.frameCount - 1];
        previous.times.nextBegin = now;
        previous.times.closed = true;
        const uint64_t previousSerial = previous.context.resourceFrameSerial;
        if (previousSerial == UINT64_MAX || resourceFrameSerial != previousSerial + 1) {
            capture.invalidBoundaries.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (capture.frameCount >= capture.samples + CAPTURE_ANCHOR_COUNT) {
        capture.invalidBoundaries.fetch_add(1);
        s_mainFrameIndex = INVALID_INDEX;
        return;
    }

    s_mainFrameIndex = capture.frameCount++;
    auto& row = capture.frames[s_mainFrameIndex];
    row.context = s_context;
    row.times.begin = now;
    row.profileBeginMs = profileStartMs;
    row.osThreadId = OsThreadId();
    row.role = role;
    if (!std::isfinite(profileStartMs)) {
        capture.invalidBoundaries.fetch_add(1);
    }

    if (role == detail::FrameRole::CLOSING_ANCHOR) {
        capture.closingAnchor = true;
        return;
    }
    if (role == detail::FrameRole::SAMPLE) {
        ++capture.sampleCount;
    }
    s_mainFrameTicket = BeginAt(capture, Event::MAIN_FRAME, s_context, 0, true, now);
}

void EndApplicationFrame(double profileEndMs)
{
    if (!IsEnabled() || s_mainFrameIndex == INVALID_INDEX || s_mainFrameTicket == 0) return;

    auto& capture = *State().capture;
    const uint64_t now = ReadQpc(capture);
    auto& row = capture.frames[s_mainFrameIndex];
    row.times.end = now;
    row.times.ended = true;
    row.profileEndMs = profileEndMs;
    if (!std::isfinite(profileEndMs) || profileEndMs < row.profileBeginMs) {
        capture.invalidBoundaries.fetch_add(1);
    }

    EndAt(capture, s_mainFrameTicket, 0, now);
    s_mainFrameTicket = 0;
}

bool WriteAfterStop()
{
    if (!IsEnabled()) return false;
    auto& state = State();
    auto& capture = *state.capture;
    if (capture.exported || capture.closing.exchange(true, std::memory_order_acq_rel)) return false;
    if (capture.activeWriters.load(std::memory_order_acquire) != 0
        || capture.activeScopes.load(std::memory_order_acquire) != 0) {
        capture.closing.store(false, std::memory_order_release);
        return false;
    }

    if (!detail::PublishCaptureFiles(capture, state.output)) return false;

    capture.exported = true;
    state.enabled.store(false, std::memory_order_release);
    return true;
}

} /// @note namespace fbzz::profiler::frame_timeline
