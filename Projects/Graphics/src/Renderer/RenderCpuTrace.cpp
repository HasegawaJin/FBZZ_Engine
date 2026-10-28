/// @file    RenderCpuTrace.cpp
/// @brief   Bounded opt-in CSV capture separating elapsed waits, OS CPU execution, and worker packets.
/// @author  Hasegawa Jin
/// @date    2026-10-13
#include <Graphics/Renderer/RenderCpuTrace.hpp>
#include <Core/Util/FileSystem.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <realtimeapiset.h>
#endif

namespace fbzz::renderer::cpu_trace {
namespace {

constexpr uint32_t INVALID_ROW = UINT32_MAX, DOMAIN_TABLE_SIZE = MAX_DOMAINS * 2;
constexpr size_t COUNTER_COUNT = static_cast<size_t>(Counter::COUNT);
using Context = detail::Context;
using Stamp = detail::Stamp;

struct ThreadEntry {
    const void* address = nullptr;
    uint64_t id = 0, framesSeen = 0, serial = 0;
    uint32_t pendingPacket = INVALID_ROW;
    bool stopped = false;
};
struct Interval {
    uint64_t wall = 0, kernel = 0, user = 0, cycles = 0;
    bool cpu = false, cyclesAvailable = false;
};
struct PhaseRow { Context context{}; Interval interval{}; bool complete = false; };
struct DomainRow {
    Context context{};
    std::array<uint64_t, COUNTER_COUNT> counters{};
    uint64_t calls = 0, notReady = 0, inclusive = 0, leaf = 0, pumpCalls = 0, pump = 0;
    uint64_t executionAcquire = 0, threadAcquire = 0;
    uint64_t maxWaitDepth = 0, reentrantWaits = 0;
    bool leafAvailable = true;
};
struct PacketRow {
    Context context{};
    uint64_t serial = 0, commands = 0;
    Interval execute{}, destroy{};
    bool accepted = false, started = false, executeFinished = false, destroyFinished = false, complete = false;
};
struct Capture {
    uint32_t warmup = 210, stride = 15, samples = 12, packetLimit = MAX_PACKETS;
    TestCpuMode cpuMode = TestCpuMode::NATIVE;
    std::string output;
    std::array<ThreadEntry, MAX_THREADS> threads{};
    std::array<PhaseRow, MAX_PHASES> phases{};
    std::array<DomainRow, MAX_DOMAINS> domains{};
    std::array<uint32_t, DOMAIN_TABLE_SIZE> domainTable{};
    std::array<PacketRow, MAX_PACKETS> packets{};
    std::array<std::array<char, MAX_API_ORIGIN_BYTES>, MAX_API_ORIGINS> apis{};
    uint32_t frames = 0, views = 0, phaseCount = 0, domainCount = 0, packetCount = 0, apiCount = 1;
    uint64_t dropped = 0, activeScopes = 0, activeWorkers = 0;
    bool incomplete = false, exported = false, exporting = false, timeMutex = false;
};
struct Global {
    std::mutex mutex;
    std::unique_ptr<Capture> capture;
    std::atomic<bool> enabled{false};
    std::atomic<uint64_t> lifetimeId{0}, cpuQueries{0}, wallQueries{0}, cycleQueries{0}, fakeCpuTicks{1000};
    bool environmentInitialized = false;
};

Global& State() { static Global state; return state; }
thread_local Context s_context{};
thread_local WaitScope* s_wait = nullptr;

/// @note The collector mutex never calls RHI, acquires render/execution mutexes, or waits on a worker.
/// @note Every shared row write/read uses this mutex. Packet completion is published before the existing worker notification.
void Drop(Capture& capture)
{
    capture.incomplete = true;
    if (capture.dropped != std::numeric_limits<uint64_t>::max()) ++capture.dropped;
}

void Add(Capture& capture, uint64_t& value, uint64_t amount)
{
    if (amount > std::numeric_limits<uint64_t>::max() - value) { value = std::numeric_limits<uint64_t>::max(); Drop(capture); }
    else value += amount;
}

uint64_t WallNow()
{
    State().wallQueries.fetch_add(1, std::memory_order_relaxed);
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t CurrentThreadIdentity()
{
#if defined(_WIN32)
    return GetCurrentThreadId();
#else
    return static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

Stamp ReadStamp()
{
    Stamp stamp;
    stamp.wall = WallNow();
    stamp.thread = CurrentThreadIdentity();
    auto& state = State();
    state.cpuQueries.fetch_add(1, std::memory_order_relaxed);
    state.cycleQueries.fetch_add(1, std::memory_order_relaxed);
    const auto mode = state.capture->cpuMode;
    if (mode == TestCpuMode::UNAVAILABLE) return stamp;
    if (mode == TestCpuMode::REGRESSING) {
        stamp.kernel = state.fakeCpuTicks.fetch_sub(1, std::memory_order_relaxed);
        stamp.user = stamp.kernel;
        stamp.cycles = stamp.kernel;
        stamp.cpuAvailable = true;
        stamp.cyclesAvailable = true;
        return stamp;
    }
#if defined(_WIN32)
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) {
        stamp.kernel = (static_cast<uint64_t>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
        stamp.user = (static_cast<uint64_t>(user.dwHighDateTime) << 32) | user.dwLowDateTime;
        stamp.cpuAvailable = true;
    }
    /// @note Query only at the existing coarse boundaries. Cycles are independent of GetThreadTimes' quantization.
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime
    ULONG64 cycles = 0;
    if (QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
        stamp.cycles = cycles;
        stamp.cyclesAvailable = true;
    }
#endif
    return stamp;
}

Interval Difference(const Stamp& before, const Stamp& after)
{
    Interval result;
    result.wall = after.wall >= before.wall ? after.wall - before.wall : 0;
    result.cpu = before.cpuAvailable && after.cpuAvailable && before.thread == after.thread
        && after.kernel >= before.kernel && after.user >= before.user;
    if (result.cpu) { result.kernel = after.kernel - before.kernel; result.user = after.user - before.user; }
    result.cyclesAvailable = before.cyclesAvailable && after.cyclesAvailable && before.thread == after.thread
        && after.cycles >= before.cycles;
    if (result.cyclesAvailable) result.cycles = after.cycles - before.cycles;
    return result;
}

ThreadEntry* FindThread(Capture& capture, const void* address)
{
    for (auto& entry : capture.threads) if (entry.address == address) return &entry;
    return nullptr;
}

ThreadEntry* Register(Capture& capture, const void* address)
{
    if (auto* entry = FindThread(capture, address)) return entry;
    for (auto& entry : capture.threads) {
        if (entry.address) continue;
        entry = {};
        entry.address = address;
        entry.id = State().lifetimeId.fetch_add(1, std::memory_order_relaxed) + 1;
        return &entry;
    }
    Drop(capture);
    return nullptr;
}

uint16_t InternApi(Capture& capture, const char* api)
{
    if (!api) return s_context.api;
    size_t size = 0;
    while (size < MAX_API_ORIGIN_BYTES && api[size]) ++size;
    if (size == MAX_API_ORIGIN_BYTES) { Drop(capture); return 0; }
    for (uint32_t i = 1; i < capture.apiCount; ++i)
        if (std::strcmp(capture.apis[i].data(), api) == 0) return static_cast<uint16_t>(i);
    if (capture.apiCount == MAX_API_ORIGINS) { Drop(capture); return 0; }
    const auto index = capture.apiCount++;
    std::memcpy(capture.apis[index].data(), api, size + 1);
    return static_cast<uint16_t>(index);
}

uint64_t Mix(uint64_t hash, uint64_t value) { return (hash ^ value) * 1099511628211ULL; }
bool SameDomain(const Context& a, const Context& b)
{
    return a.threadId == b.threadId && a.token == b.token && a.viewOrdinal == b.viewOrdinal
        && a.phase == b.phase && a.source == b.source && a.api == b.api && a.waitKind == b.waitKind && a.worker == b.worker;
}

uint32_t Domain(Capture& capture, const Context& context)
{
    uint64_t hash = Mix(Mix(Mix(1469598103934665603ULL, context.threadId), context.token), context.viewOrdinal);
    hash = Mix(Mix(Mix(Mix(Mix(hash, static_cast<uint8_t>(context.phase)), static_cast<uint8_t>(context.source)),
        context.api), static_cast<uint8_t>(context.waitKind)), context.worker);
    for (uint32_t probe = 0; probe < DOMAIN_TABLE_SIZE; ++probe) {
        auto& slot = capture.domainTable[(hash + probe) % DOMAIN_TABLE_SIZE];
        if (slot && SameDomain(capture.domains[slot - 1].context, context)) return slot - 1;
        if (slot) continue;
        if (capture.domainCount == MAX_DOMAINS) { Drop(capture); return INVALID_ROW; }
        const auto row = capture.domainCount++;
        capture.domains[row].context = context;
        slot = row + 1;
        return row;
    }
    Drop(capture);
    return INVALID_ROW;
}

uint32_t CurrentDomain()
{
    auto& state = State();
    std::lock_guard lock(state.mutex);
    return Domain(*state.capture, s_context);
}

uint32_t NewPhase(Capture& capture, Phase phase)
{
    if (capture.phaseCount == MAX_PHASES) { Drop(capture); return INVALID_ROW; }
    const auto row = capture.phaseCount++;
    capture.phases[row].context = s_context;
    capture.phases[row].context.phase = phase;
    return row;
}

void EndPhase(uint32_t row, const Stamp& start, const Stamp& end)
{
    if (row == INVALID_ROW) return;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    auto& phase = state.capture->phases[row];
    phase.interval = Difference(start, end);
    phase.context.viewId = s_context.viewId;
    phase.complete = true;
}

std::string Environment(const char* name)
{
#if defined(_MSC_VER)
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) return {};
    std::string result(value);
    std::free(value);
    return result;
#else
    const auto* value = std::getenv(name);
    return value ? value : "";
#endif
}

uint32_t BoundedSetting(const char* name, uint32_t fallback, uint32_t minimum, uint32_t maximum)
{
    const auto value = Environment(name);
    if (value.empty()) return fallback;
    uint64_t parsed = 0;
    for (const auto character : value) {
        if (character < '0' || character > '9') return fallback;
        parsed = parsed * 10 + static_cast<uint32_t>(character - '0');
        if (parsed > maximum) return fallback;
    }
    return parsed >= minimum ? static_cast<uint32_t>(parsed) : fallback;
}

const char* PhaseName(Phase phase)
{
    constexpr std::array names{"outside", "setup", "skin", "extract", "environment-pipeline", "graph", "post-cleanup", "view-inclusive"};
    return names[static_cast<size_t>(phase)];
}
const char* SourceName(Source source)
{
    constexpr std::array names{"unknown", "buffer-get", "batch-static", "batch-deformed", "batch-flush"};
    return names[static_cast<size_t>(source)];
}
const char* WaitName(WaitKind kind)
{
    constexpr std::array names{"unspecified", "slot", "acceptance", "completion", "drain"};
    return names[static_cast<size_t>(kind)];
}

std::string Quote(const char* text)
{
    std::string result = "\"";
    for (const char* character = text; *character; ++character) { if (*character == '"') result += '"'; result += *character; }
    return result + '"';
}

void Prefix(std::ostringstream& output, const Capture& capture, const char* type, const Context& context)
{
    output << type << ',' << context.threadId << ',' << context.applicationFrame << ',' << context.resourceFrame << ','
        << context.token << ',' << context.viewOrdinal << ',' << context.parentViewOrdinal << ',' << context.viewId << ',' << context.resourceInstance << ','
        << context.resourceEpoch << ',' << context.sceneGeneration << ',' << context.outputId << ',' << context.outputGeneration << ','
        << (context.worker ? "worker" : "producer") << ',' << context.depth << ',' << PhaseName(context.phase) << ','
        << SourceName(context.source) << ',' << Quote(capture.apis[context.api].data()) << ',' << WaitName(context.waitKind) << ',';
}

/// @note Rows share one schema. Generic value columns are named by metric; packet execute/destruction remain separate rows.
void Metric(std::ostringstream& output, const Capture& capture, const char* type, const Context& context,
    const char* metric, uint64_t value, uint64_t auxiliary = 0, bool available = true,
    uint64_t serial = 0, uint64_t commands = 0, bool accepted = false, bool complete = true)
{
    Prefix(output, capture, type, context);
    output << metric << ',' << value << ',' << auxiliary << ',' << available << ',' << serial << ',' << commands << ','
        << accepted << ',' << complete << ',' << capture.incomplete << ',' << capture.dropped << '\n';
}

void IntervalRows(std::ostringstream& output, const Capture& capture, const char* type, const Context& context,
    const Interval& interval, bool complete, uint64_t serial = 0, uint64_t commands = 0, bool accepted = false,
    bool elapsedAvailable = true)
{
    Metric(output, capture, type, context, "wall-ns", interval.wall, 0, complete && elapsedAvailable, serial, commands, accepted, complete);
    Metric(output, capture, type, context, "kernel-100ns", interval.kernel, 0, interval.cpu, serial, commands, accepted, complete);
    Metric(output, capture, type, context, "user-100ns", interval.user, 0, interval.cpu, serial, commands, accepted, complete);
    Metric(output, capture, type, context, "cpu-cycles", interval.cycles, 0,
        complete && elapsedAvailable && interval.cyclesAvailable, serial, commands, accepted, complete);
}

} /// @note namespace

bool IsEnabled() { return State().enabled.load(std::memory_order_acquire); }
bool HasSelectedFrame() { return IsEnabled() && s_context.token != 0; }

void InitializeFromEnvironment()
{
    auto& state = State();
    std::lock_guard lock(state.mutex);
    if (state.environmentInitialized) return;
    state.environmentInitialized = true;
    const auto output = Environment("FBZZ_RENDER_CPU_TRACE_OUTPUT");
    if (output.empty()) return;
    state.capture = std::make_unique<Capture>();
    auto& capture = *state.capture;
    capture.output = output;
    capture.warmup = BoundedSetting("FBZZ_RENDER_CPU_TRACE_WARMUP", 210, 0, 1000000);
    capture.stride = BoundedSetting("FBZZ_RENDER_CPU_TRACE_STRIDE", 15, 1, 10000);
    capture.samples = BoundedSetting("FBZZ_RENDER_CPU_TRACE_SAMPLES", 12, 1, MAX_SELECTED_FRAMES);
    capture.timeMutex = Environment("FBZZ_RENDER_CPU_TRACE_MUTEX_TIMING") == "1";
    state.enabled.store(true, std::memory_order_release);
}

void RegisterThread(const void* thread)
{
    if (!IsEnabled()) return;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    if (!state.capture->exported && !state.capture->exporting) (void)Register(*state.capture, thread);
}

void UnregisterThread(const void* thread)
{
    if (!IsEnabled()) return;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    if (auto* entry = FindThread(*state.capture, thread)) {
        if (!entry->stopped) Drop(*state.capture);
        *entry = {};
    }
}

void ThreadStopped(const void* thread)
{
    if (!IsEnabled()) return;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    if (auto* entry = FindThread(*state.capture, thread)) entry->stopped = true;
}

void BeginFrame(const void* thread, uint64_t applicationFrame)
{
    if (!IsEnabled()) return;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    auto& capture = *state.capture;
    s_context = {};
    if (capture.exported || capture.exporting) return;
    auto* entry = Register(capture, thread);
    if (!entry) return;
    const auto ordinal = entry->framesSeen++;
    if (ordinal < capture.warmup || (ordinal - capture.warmup) % capture.stride || capture.frames >= capture.samples) return;
    s_context.threadId = entry->id;
    s_context.applicationFrame = applicationFrame;
    s_context.token = ++capture.frames;
}

void Count(Counter counter, uint64_t amount)
{
    if (!HasSelectedFrame()) return;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    const auto row = Domain(*state.capture, s_context);
    if (row != INVALID_ROW) Add(*state.capture, state.capture->domains[row].counters[static_cast<size_t>(counter)], amount);
}

SourceScope::SourceScope(Source source, const char* api, bool preserveSource)
{
    if (!HasSelectedFrame()) return;
    m_active = true;
    m_previousSource = s_context.source;
    m_previousApi = s_context.api;
    if (!preserveSource || s_context.source == Source::UNKNOWN) s_context.source = source;
    if (api) {
        auto& state = State();
        std::lock_guard lock(state.mutex);
        s_context.api = InternApi(*state.capture, api);
    }
    {
        auto& state = State();
        std::lock_guard lock(state.mutex);
        ++state.capture->activeScopes;
    }
    if (!preserveSource) Count(Counter::SOURCE_OPERATIONS);
}
SourceScope::~SourceScope()
{
    if (!m_active) return;
    s_context.source = m_previousSource;
    s_context.api = m_previousApi;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    --state.capture->activeScopes;
}
WaitKindScope::WaitKindScope(WaitKind kind)
{
    if (!HasSelectedFrame()) return;
    m_active = true;
    m_previous = s_context.waitKind;
    s_context.waitKind = kind;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    ++state.capture->activeScopes;
}
WaitKindScope::~WaitKindScope()
{
    if (!m_active) return;
    s_context.waitKind = m_previous;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    --state.capture->activeScopes;
}

ViewScope::ViewScope(uint64_t resourceInstance, uint64_t resourceEpoch, uint64_t resourceFrame,
    uint64_t sceneGeneration, uint32_t outputId, uint32_t outputGeneration)
{
    if (!HasSelectedFrame()) return;
    auto& state = State();
    {
        std::lock_guard lock(state.mutex);
        auto& capture = *state.capture;
        if (capture.views == MAX_VIEWS || s_context.depth == MAX_REENTRY) { Drop(capture); return; }
        m_previous = s_context;
        s_context.resourceInstance = resourceInstance;
        s_context.resourceEpoch = resourceEpoch;
        s_context.resourceFrame = resourceFrame;
        s_context.sceneGeneration = sceneGeneration;
        s_context.outputId = outputId;
        s_context.outputGeneration = outputGeneration;
        s_context.parentViewOrdinal = m_previous.viewOrdinal;
        s_context.viewOrdinal = ++capture.views;
        ++s_context.depth;
        s_context.phase = Phase::SETUP;
        m_viewRow = NewPhase(capture, Phase::VIEW);
        m_phaseRow = NewPhase(capture, Phase::SETUP);
        ++capture.activeScopes;
        m_active = true;
    }
    m_viewStart = m_phaseStart = ReadStamp();
}

void ViewScope::SetViewId(uint32_t viewId) { if (m_active) s_context.viewId = viewId; }
void ViewScope::SetPhase(Phase phase)
{
    if (!m_active || s_context.phase == phase) return;
    const auto end = ReadStamp();
    EndPhase(m_phaseRow, m_phaseStart, end);
    auto& state = State();
    std::lock_guard lock(state.mutex);
    s_context.phase = phase;
    m_phaseRow = NewPhase(*state.capture, phase);
    m_phaseStart = end;
}

ViewScope::~ViewScope()
{
    if (!m_active) return;
    const auto end = ReadStamp();
    EndPhase(m_phaseRow, m_phaseStart, end);
    EndPhase(m_viewRow, m_viewStart, end);
    auto& state = State();
    std::lock_guard lock(state.mutex);
    --state.capture->activeScopes;
    s_context = m_previous;
}

WaitScope::WaitScope(bool initiallyNotReady)
{
    if (!HasSelectedFrame()) return;
    m_cell = CurrentDomain();
    auto& state = State();
    {
        std::lock_guard lock(state.mutex);
        if (m_cell == INVALID_ROW) return;
        auto& capture = *state.capture;
        auto& row = capture.domains[m_cell];
        Add(capture, row.calls, 1);
        if (!initiallyNotReady) return;
        Add(capture, row.notReady, 1);
        m_parent = s_wait;
        m_depth = m_parent ? m_parent->m_depth + 1 : 1;
        if (m_depth > row.maxWaitDepth) row.maxWaitDepth = m_depth;
        if (m_parent) Add(capture, row.reentrantWaits, 1);
        if (m_depth > MAX_REENTRY) {
            Drop(capture);
            row.leafAvailable = false;
            for (auto* parent = m_parent; parent; parent = parent->m_parent) parent->m_leafAvailable = false;
            return;
        }
        ++capture.activeScopes;
    }
    m_start = WallNow();
    m_timed = true;
    s_wait = this;
}

WaitScope::~WaitScope()
{
    if (!m_timed) return;
    const auto end = WallNow();
    const auto elapsed = end >= m_start ? end - m_start : 0;
    s_wait = m_parent;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    auto& capture = *state.capture;
    auto& row = capture.domains[m_cell];
    Add(capture, row.inclusive, elapsed);
    m_leafAvailable = m_leafAvailable && elapsed >= m_children;
    row.leafAvailable = row.leafAvailable && m_leafAvailable;
    if (m_leafAvailable) Add(capture, row.leaf, elapsed - m_children);
    else Drop(capture);
    if (m_parent) Add(capture, m_parent->m_children, elapsed);
    --capture.activeScopes;
}

PumpScope::PumpScope()
{
    if (!s_wait) return;
    m_cell = s_wait->m_cell;
    m_start = WallNow();
}
PumpScope::~PumpScope()
{
    if (m_cell == INVALID_ROW) return;
    const auto elapsed = WallNow() - m_start;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    auto& capture = *state.capture;
    Add(capture, capture.domains[m_cell].pumpCalls, 1);
    Add(capture, capture.domains[m_cell].pump, elapsed);
}

AcquireScope::AcquireScope(AcquireKind kind) : m_kind(kind)
{
    if (!HasSelectedFrame()) return;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    if (!state.capture->timeMutex) return;
    m_cell = Domain(*state.capture, s_context);
    if (m_cell != INVALID_ROW) {
        ++state.capture->activeScopes;
        m_start = WallNow();
    }
}
AcquireScope::~AcquireScope() { Acquired(); }
void AcquireScope::Acquired()
{
    if (m_cell == INVALID_ROW) return;
    const auto elapsed = WallNow() - m_start;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    auto& row = state.capture->domains[m_cell];
    Add(*state.capture, m_kind == AcquireKind::EXECUTION_MUTEX ? row.executionAcquire : row.threadAcquire, elapsed);
    --state.capture->activeScopes;
    m_cell = INVALID_ROW;
}

void BindPacket(const void* thread, uint64_t serial, uint64_t commands)
{
    if (!HasSelectedFrame()) return;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    auto& capture = *state.capture;
    const auto domain = Domain(capture, s_context);
    if (domain != INVALID_ROW) {
        Add(capture, capture.domains[domain].counters[static_cast<size_t>(Counter::PACKETS)], 1);
        Add(capture, capture.domains[domain].counters[static_cast<size_t>(Counter::COMMANDS)], commands);
    }
    auto* entry = Register(capture, thread);
    if (!entry) return;
    if (capture.packetCount == capture.packetLimit) { Drop(capture); entry->pendingPacket = INVALID_ROW; return; }
    if (entry->pendingPacket != INVALID_ROW && !capture.packets[entry->pendingPacket].complete) { Drop(capture); return; }
    const auto row = capture.packetCount++;
    entry->pendingPacket = row;
    entry->serial = serial;
    auto& packet = capture.packets[row];
    packet.context = s_context;
    packet.context.threadId = entry->id;
    packet.serial = serial;
    packet.commands = commands;
}

void PacketAccepted(const void* thread, uint64_t serial, bool accepted)
{
    if (!IsEnabled()) return;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    auto& capture = *state.capture;
    auto* entry = FindThread(capture, thread);
    if (!entry || entry->serial != serial || entry->pendingPacket == INVALID_ROW) return;
    auto& packet = capture.packets[entry->pendingPacket];
    packet.accepted = accepted;
    if (!accepted) { packet.complete = true; entry->pendingPacket = INVALID_ROW; }
}

PacketScope::PacketScope(const void* thread, uint64_t serial)
{
    if (!IsEnabled()) return;
    auto& state = State();
    {
        std::lock_guard lock(state.mutex);
        auto& capture = *state.capture;
        auto* entry = FindThread(capture, thread);
        if (!entry || entry->serial != serial || entry->pendingPacket == INVALID_ROW) return;
        m_row = entry->pendingPacket;
        m_previous = s_context;
        s_context = capture.packets[m_row].context;
        s_context.worker = true;
        capture.packets[m_row].started = true;
        ++capture.activeWorkers;
    }
    m_start = ReadStamp();
}

void PacketScope::BeginDestruction()
{
    if (m_row == INVALID_ROW) return;
    const auto end = ReadStamp();
    auto& state = State();
    std::lock_guard lock(state.mutex);
    state.capture->packets[m_row].execute = Difference(m_start, end);
    state.capture->packets[m_row].executeFinished = true;
    m_start = end;
    m_destroying = true;
}
void PacketScope::FinishedDestruction()
{
    if (m_row == INVALID_ROW) return;
    const auto end = ReadStamp();
    auto& state = State();
    std::lock_guard lock(state.mutex);
    auto& capture = *state.capture;
    auto& row = capture.packets[m_row];
    row.destroy = Difference(m_start, end);
    row.complete = m_destroying;
    row.destroyFinished = m_destroying;
    if (!m_destroying) Drop(capture);
    --capture.activeWorkers;
    s_context = m_previous;
    m_finished = true;
}
PacketScope::~PacketScope()
{
    if (m_row == INVALID_ROW || m_finished) return;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    Drop(*state.capture);
    --state.capture->activeWorkers;
    s_context = m_previous;
}

Summary SnapshotSummary()
{
    Summary summary;
    auto& state = State();
    std::lock_guard lock(state.mutex);
    if (!state.capture) return summary;
    const auto& capture = *state.capture;
    summary.selectedFrames = capture.frames;
    summary.views = capture.views;
    summary.phases = capture.phaseCount;
    summary.packets = capture.packetCount;
    summary.dropped = capture.dropped;
    summary.cpuQueries = state.cpuQueries.load(std::memory_order_relaxed);
    summary.wallQueries = state.wallQueries.load(std::memory_order_relaxed);
    summary.cycleQueries = state.cycleQueries.load(std::memory_order_relaxed);
    summary.complete = !capture.incomplete && capture.frames == capture.samples && !capture.activeScopes && !capture.activeWorkers;
    for (uint32_t i = 0; i < capture.domainCount; ++i) {
        const auto& row = capture.domains[i];
        summary.waitCalls += row.calls;
        summary.notReady += row.notReady;
        summary.waitInclusiveNs += row.inclusive;
        summary.waitLeafNs += row.leaf;
        summary.pumpCalls += row.pumpCalls;
        summary.leafAvailable &= row.leafAvailable;
        summary.bufferProducer += row.counters[static_cast<size_t>(Counter::BUFFER_PRODUCER)];
        summary.bufferWorker += row.counters[static_cast<size_t>(Counter::BUFFER_WORKER)];
        summary.emptySubmits += row.counters[static_cast<size_t>(Counter::EMPTY_SUBMIT)];
        if (row.context.source == Source::BATCH_STATIC) summary.sourceStaticCalls += row.counters[static_cast<size_t>(Counter::SOURCE_OPERATIONS)];
        if (row.context.source == Source::BATCH_DEFORMED) summary.sourceDeformedCalls += row.counters[static_cast<size_t>(Counter::SOURCE_OPERATIONS)];
    }
    for (uint32_t i = 0; i < capture.phaseCount; ++i) {
        summary.complete &= capture.phases[i].complete;
        summary.cpuAvailable &= capture.phases[i].interval.cpu;
        summary.cyclesAvailable &= capture.phases[i].interval.cyclesAvailable;
    }
    for (uint32_t i = 0; i < capture.packetCount; ++i) {
        const auto& row = capture.packets[i];
        if (row.accepted && row.complete) ++summary.completedPackets;
        summary.complete &= row.complete;
        if (row.started) {
            summary.cpuAvailable &= row.execute.cpu && row.destroy.cpu;
            summary.cyclesAvailable &= row.execute.cyclesAvailable && row.destroy.cyclesAvailable;
        }
    }
    return summary;
}

PacketInfo SnapshotPacket(uint32_t index)
{
    auto& state = State();
    std::lock_guard lock(state.mutex);
    if (!state.capture || index >= state.capture->packetCount) return {};
    const auto& packet = state.capture->packets[index];
    return {packet.context.threadId, packet.context.applicationFrame, packet.context.resourceFrame, packet.serial,
        packet.context.token, packet.context.viewOrdinal, packet.context.viewId, packet.accepted, packet.complete};
}

bool WriteCsvAfterStop()
{
    if (!IsEnabled()) return false;
    auto& state = State();
    std::string outputPath, contents;
    {
        std::lock_guard lock(state.mutex);
        auto& capture = *state.capture;
        if (capture.exported || capture.exporting || capture.output.empty() || capture.activeScopes || capture.activeWorkers) return false;
        for (const auto& entry : capture.threads) if (entry.address && !entry.stopped) return false;
        if (capture.frames != capture.samples) capture.incomplete = true;
        for (uint32_t i = 0; i < capture.packetCount; ++i) if (!capture.packets[i].complete) capture.incomplete = true;
        std::ostringstream output;
        output << "rowType,threadLifetimeId,applicationFrame,resourceFrame,token,viewOrdinal,parentViewOrdinal,viewId,resourceInstance,resourceEpoch,sceneGeneration,outputId,outputGeneration,role,depth,phase,source,api,waitKind,metric,value,auxiliary,available,packetSerial,commandCount,accepted,rowComplete,captureIncomplete,dropped\n";
        const Context meta{};
        Metric(output, capture, "meta", meta, "selected-frames", capture.frames, capture.samples);
        Metric(output, capture, "meta", meta, "warmup", capture.warmup);
        Metric(output, capture, "meta", meta, "stride", capture.stride);
        Metric(output, capture, "meta", meta, "cpu-queries", state.cpuQueries.load(std::memory_order_relaxed));
        Metric(output, capture, "meta", meta, "wall-queries", state.wallQueries.load(std::memory_order_relaxed));
        Metric(output, capture, "meta", meta, "cycle-queries", state.cycleQueries.load(std::memory_order_relaxed));
        Metric(output, capture, "meta", meta, "os-cpu-time-coarse", 1);
        Metric(output, capture, "meta", meta, "cycle-counter-raw", 1);
        Metric(output, capture, "meta", meta, "mutex-timing", capture.timeMutex);
        for (uint32_t i = 0; i < capture.phaseCount; ++i)
            IntervalRows(output, capture, "phase", capture.phases[i].context, capture.phases[i].interval, capture.phases[i].complete);
        constexpr std::array counterNames{"sync-calls", "cpu-frame-wait-calls", "buffer-producer", "buffer-worker", "buffer-invalid",
            "buffer-detached", "empty-submit", "packets", "commands", "source-operations"};
        for (uint32_t i = 0; i < capture.domainCount; ++i) {
            const auto& row = capture.domains[i];
            Metric(output, capture, "wait", row.context, "calls", row.calls, row.notReady);
            Metric(output, capture, "wait", row.context, "inclusive-ns", row.inclusive);
            Metric(output, capture, "wait", row.context, "leaf-ns", row.leaf, 0, row.leafAvailable);
            Metric(output, capture, "wait", row.context, "pump-ns", row.pump, row.pumpCalls);
            Metric(output, capture, "wait", row.context, "max-wait-depth", row.maxWaitDepth, row.reentrantWaits);
            Metric(output, capture, "acquire", row.context, "execution-mutex-ns", row.executionAcquire, 0, capture.timeMutex);
            Metric(output, capture, "acquire", row.context, "thread-mutex-ns", row.threadAcquire, 0, capture.timeMutex);
            for (size_t counter = 0; counter < COUNTER_COUNT; ++counter)
                if (row.counters[counter]) Metric(output, capture, "counter", row.context, counterNames[counter], row.counters[counter]);
        }
        for (uint32_t i = 0; i < capture.packetCount; ++i) {
            const auto& row = capture.packets[i];
            auto context = row.context;
            context.worker = true;
            IntervalRows(output, capture, "packet-execute", context, row.execute, row.complete, row.serial, row.commands, row.accepted, row.executeFinished);
            IntervalRows(output, capture, "packet-destroy", context, row.destroy, row.complete, row.serial, row.commands, row.accepted, row.destroyFinished);
        }
        outputPath = capture.output;
        contents = output.str();
        capture.exporting = true;
    }
    const bool written = util::FileSystem::WriteTextAtomic(outputPath, contents);
    {
        std::lock_guard lock(state.mutex);
        state.capture->exporting = false;
        state.capture->exported = written;
        if (written) state.enabled.store(false, std::memory_order_release);
    }
    if (!written) std::fprintf(stderr, "FBZZ render CPU trace CSV write failed: %s\n", outputPath.c_str());
    return written;
}

bool ConfigureForTesting(uint32_t warmup, uint32_t stride, uint32_t samples, uint32_t packetLimit, TestCpuMode cpuMode,
    const char* outputPath)
{
    auto& state = State();
    std::lock_guard lock(state.mutex);
    if (state.capture && (state.capture->activeScopes || state.capture->activeWorkers || state.capture->exporting)) return false;
    if (!stride || !samples || samples > MAX_SELECTED_FRAMES || !packetLimit || packetLimit > MAX_PACKETS) return false;
    state.enabled.store(false, std::memory_order_release);
    state.capture = std::make_unique<Capture>();
    state.capture->warmup = warmup;
    state.capture->stride = stride;
    state.capture->samples = samples;
    state.capture->packetLimit = packetLimit;
    state.capture->cpuMode = cpuMode;
    if (outputPath) state.capture->output = outputPath;
    state.environmentInitialized = true;
    state.cpuQueries.store(0, std::memory_order_relaxed);
    state.wallQueries.store(0, std::memory_order_relaxed);
    state.cycleQueries.store(0, std::memory_order_relaxed);
    state.fakeCpuTicks.store(1000, std::memory_order_relaxed);
    s_context = {};
    s_wait = nullptr;
    state.enabled.store(true, std::memory_order_release);
    return true;
}

bool DisableForTesting()
{
    auto& state = State();
    std::lock_guard lock(state.mutex);
    if (state.capture && (state.capture->activeScopes || state.capture->activeWorkers || state.capture->exporting)) return false;
    state.enabled.store(false, std::memory_order_release);
    state.capture.reset();
    s_context = {};
    s_wait = nullptr;
    return true;
}

} /// @note namespace fbzz::renderer::cpu_trace
