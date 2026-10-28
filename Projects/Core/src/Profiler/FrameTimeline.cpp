/// @file    FrameTimeline.cpp
/// @brief   Fixed-capacity QPC chronology and coarse OS thread counters, exported only after shutdown.
/// @author  Hasegawa Jin
/// @date    2026-10-16
#include <Core/Profiler/FrameTimeline.hpp>
#include <Core/Util/FileSystem.hpp>
#include "FrameTimelineStorage.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
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

constexpr uint64_t INVALID_INDEX = UINT64_MAX;
constexpr size_t MAX_FRAMES = 60000;
constexpr size_t DEFAULT_EVENTS = 2000000, MAX_EVENTS = 4000000;
constexpr uint64_t DEFERRED_TICKET_BIT = uint64_t{1} << 63;
constexpr size_t MAX_PENDING_FINE_SPANS = 64;
constexpr uint64_t FINE_SPANS_PER_SECOND = 200000;
enum class Edge : uint8_t { BEGIN, END, INSTANT };
struct CpuCounters {
    uint64_t kernel = 0, user = 0, cycles = 0;
    bool cpuAvailable = false, cyclesAvailable = false;
};
struct EventRow {
    Context context{};
    uint64_t qpc = 0, scope = 0, value = 0, cpu = INVALID_INDEX;
    uint32_t osThreadId = 0;
    Event event = Event::MAIN_FRAME;
    Edge edge = Edge::INSTANT;
    bool threadCpu = false;
    std::atomic<bool> ended{false};
};
struct FrameRow {
    Context context{};
    detail::FrameTimes times;
    double profileBeginMs = 0.0, profileEndMs = 0.0;
    uint32_t osThreadId = 0;
    detail::FrameRole role = detail::FrameRole::UNSELECTED;
};
struct Capture {
    Capture(uint64_t session, size_t warmupCount, size_t samplesCount, size_t eventCount)
        : observerSession(session), warmup(warmupCount), samples(samplesCount),
          eventCapacity(eventCount), cpuCapacity((samplesCount + 2) * 4 + 64),
          window(warmupCount, samplesCount), events(std::make_unique<EventRow[]>(eventCount)),
          cpu(std::make_unique<CpuCounters[]>(cpuCapacity)), frames(std::make_unique<FrameRow[]>(samplesCount + 2)) {}
    uint64_t observerSession = 0, warmup = 0, samples = 0, frequency = 0;
    size_t eventCapacity = 0, cpuCapacity = 0;
    detail::FrameWindow window;
    std::unique_ptr<EventRow[]> events;
    std::unique_ptr<CpuCounters[]> cpu;
    std::unique_ptr<FrameRow[]> frames;
    std::atomic<uint64_t> eventCount{0}, cpuCount{0}, droppedEvents{0}, droppedCpu{0};
    std::atomic<uint64_t> activeWriters{0}, activeScopes{0}, wallQueries{0}, cpuQueries{0}, invalidBoundaries{0};
    std::array<std::atomic<uint64_t>, static_cast<size_t>(Event::COUNT)> filteredFineScopes{}, filteredFineTicks{};
    std::atomic<uint64_t> fineSpanFallbacks{0}, fineTicketSequence{0};
    std::atomic<bool> closing{false}, cancelled{false};
    size_t frameCount = 0, sampleCount = 0;
    bool closingAnchor = false, exported = false;
};
struct Global {
    std::mutex coldMutex;
    std::unique_ptr<Capture> capture;
    std::string output;
    std::atomic<bool> enabled{false};
    size_t eventCapacity = DEFAULT_EVENTS;
    bool initialized = false;
};
Global& State() { static Global state; return state; }
thread_local Context s_context{};
thread_local uint64_t s_mainTicket = 0, s_mainRow = INVALID_INDEX;
struct PendingFineSpan {
    Capture* capture = nullptr;
    Context context{};
    uint64_t ticket = 0, qpc = 0, value = 0;
    Event event = Event::MAIN_FRAME;
};
thread_local std::array<PendingFineSpan, MAX_PENDING_FINE_SPANS> s_pendingFineSpans{};

/// @note Only four high-frequency fine spans omit durations below5us; every coarse phase and other wait remains unconditional.
bool IsFineSpan(Event event)
{
    return event == Event::EXECUTION_MUTEX || event == Event::THREAD_MUTEX
        || event == Event::WAIT_SLOT || event == Event::PACKET_DESTROY;
}

uint64_t FineThresholdTicks(const Capture& capture)
{
    return capture.frequency / FINE_SPANS_PER_SECOND + (capture.frequency % FINE_SPANS_PER_SECOND != 0);
}

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
uint64_t Qpc(Capture& capture)
{
    capture.wallQueries.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
    LARGE_INTEGER value{};
    if (QueryPerformanceCounter(&value)) return static_cast<uint64_t>(value.QuadPart);
#endif
    capture.invalidBoundaries.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

/// @note FILETIME units are100ns execution counts, not wall time or a guarantee of per-frame accounting precision.
/// @see https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadtimes GetThreadTimes.
/// @see https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime Raw cycles must not be converted into elapsed time.
uint64_t ReadCpu(Capture& capture)
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

bool Selected(const Capture& capture, const Context& context)
{
    const uint64_t first = capture.warmup == 0 ? 1 : capture.warmup;
    return context.captureFrameOrdinal >= first && context.captureFrameOrdinal <= capture.warmup + capture.samples;
}

class Writer final {
public:
    explicit Writer(Capture& capture) : m_capture(capture)
    {
        m_capture.activeWriters.fetch_add(1, std::memory_order_acquire);
        m_valid = !m_capture.closing.load(std::memory_order_acquire);
    }
    ~Writer() { m_capture.activeWriters.fetch_sub(1, std::memory_order_release); }
    [[nodiscard]] bool Valid() const { return m_valid; }
private:
    Capture& m_capture;
    bool m_valid = false;
};

uint64_t Append(Capture& capture, Event event, Edge edge, Context context, uint64_t scope,
    uint64_t value, bool threadCpu, uint64_t qpc)
{
    const uint64_t index = detail::ReserveBounded(capture.eventCount, capture.eventCapacity, capture.droppedEvents);
    if (index == INVALID_INDEX) return 0;
    auto& row = capture.events[index];
    row.context = context; row.qpc = qpc; row.scope = scope; row.value = value;
    row.osThreadId = OsThreadId(); row.event = event; row.edge = edge; row.threadCpu = threadCpu;
    if (threadCpu) row.cpu = ReadCpu(capture);
    return index + 1;
}

uint64_t BeginAt(Capture& capture, Event event, Context context, uint64_t value, bool threadCpu, uint64_t qpc)
{
    const uint64_t ticket = Append(capture, event, Edge::BEGIN, context, 0, value, threadCpu, qpc);
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
        capture.invalidBoundaries.fetch_add(1, std::memory_order_relaxed); return;
    }
    (void)Append(capture, begin.event, Edge::END, begin.context, ticket, value, begin.threadCpu, qpc);
    capture.activeScopes.fetch_sub(1, std::memory_order_relaxed);
}

/// @note Fixed TLS slots preserve start timestamps without publishing two rows for subthreshold spans; exhaustion uses the original unfiltered path.
uint64_t DeferFineSpan(Capture& capture, Event event, Context context, uint64_t value, uint64_t qpc)
{
    for (size_t index = 0; index < s_pendingFineSpans.size(); ++index) {
        auto& pending = s_pendingFineSpans[index];
        if (pending.ticket != 0) continue;
        const uint64_t sequence = (capture.fineTicketSequence.fetch_add(1, std::memory_order_relaxed) + 1)
            & ((DEFERRED_TICKET_BIT - 1) >> 6);
        const uint64_t ticket = DEFERRED_TICKET_BIT | (sequence << 6) | index;
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

const char* EventName(Event event)
{
    static constexpr const char* names[]{"main-frame", "time-tick", "input", "window-events", "input-callbacks", "input-actions",
        "asset-pump", "module-update", "module-late-update", "audio-update", "module-render", "frame-tail", "editor-update",
        "simulation", "view", "skinning", "extract", "environment", "graph", "post-cleanup", "worker-frame", "packet",
        "packet-execute", "packet-destroy", "packet-submit", "packet-accept", "wait-slot", "wait-acceptance", "wait-completion",
        "wait-drain", "wait-other", "wait-pump", "execution-mutex", "thread-mutex", "fence-wait", "synchronous-fence-wait", "present", "queue-submit"};
    static_assert(std::size(names) == static_cast<size_t>(Event::COUNT));
    return static_cast<size_t>(event) < std::size(names) ? names[static_cast<size_t>(event)] : "invalid";
}

void ContextColumns(std::ostream& out, const Context& context)
{
    out << context.captureFrameOrdinal << ',' << context.applicationFrameSerial << ',' << context.resourceFrameSerial << ','
        << context.submissionSerial << ',' << context.targetSerial << ',' << context.objectId << ',' << context.viewId << ',' << context.detail;
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
    m_previous = s_context; s_context = context; m_active = true;
}
ContextScope::~ContextScope() { if (m_active) s_context = m_previous; }

uint64_t Begin(Event event, Context context, uint64_t value, bool threadCpu)
{
    if (!IsEnabled()) return 0;
    auto& capture = *State().capture;
    if (context.captureFrameOrdinal == 0) context = s_context;
    if (!Selected(capture, context) || capture.cancelled.load(std::memory_order_acquire)) return 0;
    const Writer writer(capture);
    if (!writer.Valid()) return 0;
    const uint64_t now = Qpc(capture);
    return !threadCpu && IsFineSpan(event) ? DeferFineSpan(capture, event, context, value, now)
        : BeginAt(capture, event, context, value, threadCpu, now);
}

void End(uint64_t ticket, uint64_t value)
{
    if (ticket == 0 || !IsEnabled()) return;
    auto& capture = *State().capture;
    const Writer writer(capture);
    if (writer.Valid()) {
        const uint64_t now = Qpc(capture);
        if ((ticket & DEFERRED_TICKET_BIT) != 0) EndFineSpan(capture, ticket, value, now);
        else EndAt(capture, ticket, value, now);
    }
}

void Mark(Event event, Context context, uint64_t value)
{
    if (!IsEnabled()) return;
    auto& capture = *State().capture;
    if (context.captureFrameOrdinal == 0) context = s_context;
    if (!Selected(capture, context) || capture.cancelled.load(std::memory_order_acquire)) return;
    const Writer writer(capture);
    if (writer.Valid()) (void)Append(capture, event, Edge::INSTANT, context, 0, value, false, Qpc(capture));
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
    if (role == detail::FrameRole::UNSELECTED || role == detail::FrameRole::FINISHED) { s_mainRow = INVALID_INDEX; return; }
    const uint64_t now = Qpc(capture);
    if (s_mainTicket != 0) { EndAt(capture, s_mainTicket, 0, now); s_mainTicket = 0; capture.invalidBoundaries.fetch_add(1); }
    if (capture.frameCount > 0) {
        auto& previous = capture.frames[capture.frameCount - 1];
        previous.times.nextBegin = now; previous.times.closed = true;
        if (previous.context.resourceFrameSerial == UINT64_MAX || resourceFrameSerial != previous.context.resourceFrameSerial + 1)
            capture.invalidBoundaries.fetch_add(1, std::memory_order_relaxed);
    }
    if (capture.frameCount >= capture.samples + 2) { capture.invalidBoundaries.fetch_add(1); s_mainRow = INVALID_INDEX; return; }
    s_mainRow = capture.frameCount++;
    auto& row = capture.frames[s_mainRow];
    row.context = s_context; row.times.begin = now; row.profileBeginMs = profileStartMs;
    row.osThreadId = OsThreadId(); row.role = role;
    if (!std::isfinite(profileStartMs)) capture.invalidBoundaries.fetch_add(1);
    if (role == detail::FrameRole::CLOSING_ANCHOR) { capture.closingAnchor = true; return; }
    if (role == detail::FrameRole::SAMPLE) ++capture.sampleCount;
    s_mainTicket = BeginAt(capture, Event::MAIN_FRAME, s_context, 0, true, now);
}

void EndApplicationFrame(double profileEndMs)
{
    if (!IsEnabled() || s_mainRow == INVALID_INDEX || s_mainTicket == 0) return;
    auto& capture = *State().capture;
    const uint64_t now = Qpc(capture);
    auto& row = capture.frames[s_mainRow];
    row.times.end = now; row.times.ended = true; row.profileEndMs = profileEndMs;
    if (!std::isfinite(profileEndMs) || profileEndMs < row.profileBeginMs) capture.invalidBoundaries.fetch_add(1);
    EndAt(capture, s_mainTicket, 0, now); s_mainTicket = 0;
}

bool WriteAfterStop()
{
    if (!IsEnabled()) return false;
    auto& state = State();
    auto& capture = *state.capture;
    if (capture.exported || capture.closing.exchange(true, std::memory_order_acq_rel)) return false;
    if (capture.activeWriters.load(std::memory_order_acquire) != 0 || capture.activeScopes.load(std::memory_order_acquire) != 0) {
        capture.closing.store(false, std::memory_order_release); return false;
    }
    bool complete = capture.closingAnchor && capture.sampleCount == capture.samples
        && !capture.cancelled.load() && capture.droppedEvents.load() == 0 && capture.droppedCpu.load() == 0 && capture.invalidBoundaries.load() == 0;
    for (size_t index = 0; index < capture.frameCount; ++index) {
        const auto& row = capture.frames[index];
        if (row.role != detail::FrameRole::CLOSING_ANCHOR) complete &= row.times.TailAvailable();
    }
    const auto outputPath = util::FileSystem::PathFromUtf8(state.output);
    auto pendingPath = outputPath; pendingPath += ".pending";
    auto metadataPath = outputPath; metadataPath += ".meta.json";
    std::error_code error;
    if (std::filesystem::exists(outputPath, error) || error || std::filesystem::exists(metadataPath, error) || error
        || std::filesystem::exists(pendingPath, error) || error) return false;
    std::ofstream out(pendingPath, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.imbue(std::locale::classic());
    out << std::setprecision(17);
    out << "rowType,captureFrameOrdinal,applicationFrameSerial,resourceFrameSerial,submissionSerial,targetSerial,objectId,viewId,detail,osThreadId,event,edge,scope,qpc,value,cpuAvailable,kernel100ns,user100ns,cyclesAvailable,cycles,role,beginQpc,endQpc,nextBeginQpc,profileBeginMs,profileEndMs,cpuElapsedAvailable,forwardIntervalAvailable,tailAvailable\n";
    for (size_t index = 0; index < capture.frameCount; ++index) {
        const auto& row = capture.frames[index];
        out << "frame,"; ContextColumns(out, row.context);
        out << ',' << row.osThreadId << ",main-frame,anchor,0," << row.times.begin << ",0,0,0,0,0,0,"
            << static_cast<unsigned>(row.role) << ',' << row.times.begin << ',' << row.times.end << ',' << row.times.nextBegin << ','
            << row.profileBeginMs << ',' << row.profileEndMs << ',' << row.times.CpuAvailable() << ',' << row.times.IntervalAvailable() << ',' << row.times.TailAvailable() << '\n';
    }
    const uint64_t events = (std::min)(capture.eventCount.load(), static_cast<uint64_t>(capture.eventCapacity));
    for (uint64_t index = 0; index < events; ++index) {
        const auto& row = capture.events[index];
        const CpuCounters counters = row.cpu != INVALID_INDEX ? capture.cpu[row.cpu] : CpuCounters{};
        out << "event,"; ContextColumns(out, row.context);
        out << ',' << row.osThreadId << ',' << EventName(row.event) << ',' << (row.edge == Edge::BEGIN ? "begin" : row.edge == Edge::END ? "end" : "instant") << ','
            << row.scope << ',' << row.qpc << ',' << row.value << ',' << counters.cpuAvailable << ',' << counters.kernel << ',' << counters.user << ','
            << counters.cyclesAvailable << ',' << counters.cycles << ",0,0,0,0,0,0,0,0,0\n";
    }
    out.flush();
    const bool written = out.good(); out.close();
    if (!written || out.fail()) return false;
    std::filesystem::rename(pendingPath, outputPath, error);
    if (error) return false;
    std::ofstream meta(metadataPath, std::ios::binary | std::ios::trunc);
    uint32_t processId = 0;
#if defined(_WIN32)
    processId = GetCurrentProcessId();
#endif
    meta << "{\n  \"schemaVersion\":1,\n  \"instrumentationMode\":\"frame-timeline/qpc-v1\",\n  \"observerSessionId\":" << capture.observerSession
        << ",\n  \"processId\":" << processId << ",\n  \"qpcFrequency\":" << capture.frequency << ",\n  \"requestedWarmupFrames\":" << capture.warmup << ",\n  \"requestedSampleFrames\":" << capture.samples
        << ",\n  \"sampleFrames\":" << capture.sampleCount << ",\n  \"frameAnchors\":" << capture.frameCount << ",\n  \"closingNextBegin\":" << (capture.closingAnchor ? "true" : "false")
        << ",\n  \"eventCapacity\":" << capture.eventCapacity << ",\n  \"eventRowBytes\":" << sizeof(EventRow) << ",\n  \"reservedEventBytes\":" << capture.eventCapacity * sizeof(EventRow)
        << ",\n  \"events\":" << events << ",\n  \"droppedEvents\":" << capture.droppedEvents.load()
        << ",\n  \"droppedCpuCounters\":" << capture.droppedCpu.load() << ",\n  \"invalidBoundaries\":" << capture.invalidBoundaries.load()
        << ",\n  \"wallQueries\":" << capture.wallQueries.load() << ",\n  \"threadCpuQueries\":" << capture.cpuQueries.load()
        << ",\n  \"threadCpuCounterApiCalls\":" << capture.cpuQueries.load() * 2
        << ",\n  \"cancelled\":" << (capture.cancelled.load() ? "true" : "false") << ",\n  \"complete\":" << (complete ? "true" : "false")
        << ",\n  \"fineSpanFilter\":{\"version\":1,\"minimumRetainedMicroseconds\":5,\"minimumRetainedTicks\":" << FineThresholdTicks(capture)
        << ",\"pendingSlotsPerThread\":" << MAX_PENDING_FINE_SPANS << ",\"unfilteredFallbackScopes\":" << capture.fineSpanFallbacks.load()
        << ",\"threadCpuScopesExempt\":true,\"countsAreOverlappingScopesNotWallTime\":true,\"events\":[";
    bool firstFine = true;
    for (size_t index = 0; index < static_cast<size_t>(Event::COUNT); ++index) {
        if (!IsFineSpan(static_cast<Event>(index))) continue;
        if (!firstFine) meta << ',';
        firstFine = false;
        meta << "{\"event\":\"" << EventName(static_cast<Event>(index)) << "\",\"omittedScopes\":" << capture.filteredFineScopes[index].load()
            << ",\"omittedDurationTicks\":" << capture.filteredFineTicks[index].load() << '}';
    }
    meta << "]},\n  \"eventPublicationOrder\":\"Begin precedes its matching End; fine spans may publish after later QPC events\""
        << ",\n  \"frameInterval\":\"frameF.beginQpc to frameF.nextBeginQpc; passive wall row F+1\",\n  \"applicationFrameSerialDomain\":\"Time::frameCount and BeginCpuFrame producer serial\",\n  \"resourceFrameSerialDomain\":\"ResourceManager::FrameStamp; joins passive/GPU applicationFrameSerial\",\n  \"frameRoles\":{\"leading\":1,\"sample\":2,\"closingAnchor\":3},\n  \"cpuCounterUnits\":\"kernel/user100ns; raw cycles must not be converted to ms\",\n  \"overlap\":\"wait/pump/worker/present overlap; never add inclusive spans blindly\"\n}\n";
    meta.flush();
    const bool metadataWritten = meta.good(); meta.close();
    if (!metadataWritten || meta.fail()) return false;
    capture.exported = true;
    state.enabled.store(false, std::memory_order_release);
    return true;
}

} /// @note namespace fbzz::profiler::frame_timeline
