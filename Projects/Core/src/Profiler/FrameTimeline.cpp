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
constexpr size_t MIN_EVENTS = 1024;
constexpr size_t DEFAULT_EVENTS = 2000000;
constexpr size_t MAX_EVENTS = 4000000;
constexpr uint64_t DEFERRED_TICKET_BIT = uint64_t{1} << 63;
/// @brief プロセス全体で共有するタイムラインの収集設定と記録先。
struct TimelineState {
    std::mutex coldMutex;                  ///< 環境設定の読み取りと収集開始を直列化するロック。
    std::unique_ptr<Capture> capture;      ///< プロセスで一度だけ開始できる収集データの所有先。
    std::string output;                    ///< 環境変数で指定された CSV の出力先。空なら収集しない。
    std::atomic<bool> enabled{false};      ///< 初期化済みの収集データを各スレッドから参照できるとき true。
    size_t eventCapacity = DEFAULT_EVENTS; ///< 収集開始時に予約するイベント行数の上限。
    bool initialized = false;              ///< プロセスの環境設定を読み取り済みなら true。
};
TimelineState& State() { static TimelineState state; return state; }
thread_local Context s_context{};
thread_local uint64_t s_mainFrameTicket = 0;
thread_local uint64_t s_mainFrameIndex = INVALID_INDEX;
/// @brief 区間の長さが判明するまでスレッド内に保留する開始イベント。
struct PendingFineSpan {
    Capture* capture = nullptr;      ///< この区間を開始した収集データへの非所有ポインタ。
    Context context{};               ///< 開始時にコピーしたフレーム・投入元の識別情報。
    uint64_t ticket = 0;              ///< 保留枠を識別するチケット。0 は空き枠。
    uint64_t qpc = 0;                 ///< この区間の開始時刻（QPC ティック）。
    uint64_t value = 0;               ///< 区間を記録する場合に開始行へ渡すイベント固有の値。
    Event event = Event::MAIN_FRAME; ///< 短い区間を省略したときの集計に使うイベント種別。
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

/// @note QPC の生の値と周波数を残し、フレーム境界で記録した既存プロファイラの時刻（ミリ秒）と対応付ける。
/// @see https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps QueryPerformanceCounter による高分解能の時刻取得。
uint64_t ReadQpc(Capture& capture)
{
    capture.wallQueries.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
    LARGE_INTEGER value{};
    if (QueryPerformanceCounter(&value)) {
        return static_cast<uint64_t>(value.QuadPart);
    }
#endif
    capture.invalidBoundaries.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

/// @note FILETIME のスレッド実行時間は 100 ナノ秒単位で表す。実際の分解能はフレーム単位の比較を制約する。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadtimes GetThreadTimes が返すカーネル時間とユーザー時間。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime QueryThreadCycleTime のサイクル数を経過時間へ換算できない理由。
uint64_t RecordThreadCpuCounters(Capture& capture)
{
    const uint64_t index = detail::ReserveBounded(capture.cpuCount, capture.cpuCapacity, capture.droppedCpu);
    if (index == INVALID_INDEX) {
        return INVALID_INDEX;
    }
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
    if (QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
        row.cycles = cycles;
        row.cyclesAvailable = true;
    }
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
    Capture& m_capture; ///< この書き込みを受け付けるプロセスの収集領域。
    bool m_valid = false; ///< 収集停止前に書き込みを開始できた場合に true。
};

uint64_t AppendEvent(Capture& capture, Event event, Edge edge, Context context, uint64_t scope,
    uint64_t value, bool threadCpu, uint64_t qpc)
{
    const uint64_t index = detail::ReserveBounded(capture.eventCount, capture.eventCapacity, capture.droppedEvents);
    if (index == INVALID_INDEX) {
        return 0;
    }
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
    if (ticket == 0 || ticket > capture.eventCapacity) {
        return;
    }

    // 発行元のスレッドと開始行を照合してから、終了済みの印を一度だけ付ける。
    auto& begin = capture.events[ticket - 1];
    const bool startedOnThisThread = begin.edge == Edge::BEGIN && begin.osThreadId == OsThreadId();
    if (!startedOnThisThread || begin.ended.exchange(true)) {
        capture.invalidBoundaries.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // 対応する終了行の枠が尽きても、開始済み区間の終了は記録する。
    (void)AppendEvent(capture, begin.event, Edge::END, begin.context, ticket, value, begin.threadCpu, qpc);
    capture.activeScopes.fetch_sub(1, std::memory_order_relaxed);
}

/// @note 短い区間の開始時刻はスレッドごとの固定枠に保留し、しきい値未満なら開始行も終了行も作らない。枠が埋まれば通常どおり記録する。
uint64_t DeferFineSpan(Capture& capture, Event event, Context context, uint64_t value, uint64_t qpc)
{
    for (size_t index = 0; index < s_pendingFineSpans.size(); ++index) {
        auto& pending = s_pendingFineSpans[index];
        if (pending.ticket != 0) {
            continue;
        }
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

/// @note 残す区間は元の QPC 時刻を使い、開始行、終了行の順に追加する。他のイベントを含む行の順序は時刻順とは限らない。
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
    if (!text || !*text) {
        return DEFAULT_EVENTS;
    }

    // 10 進表記だけを受け付け、値を積み上げる前後で記録領域の上限を確かめる。
    size_t value = 0;
    for (const char* digit = text; *digit; ++digit) {
        const bool isDecimalDigit = *digit >= '0' && *digit <= '9';
        if (!isDecimalDigit || value > MAX_EVENTS / 10) {
            return DEFAULT_EVENTS;
        }
        value = value * 10 + static_cast<size_t>(*digit - '0');
        if (value > MAX_EVENTS) {
            return DEFAULT_EVENTS;
        }
    }
    return value >= MIN_EVENTS ? value : DEFAULT_EVENTS;
}

} // namespace

void InitializeFromEnvironment()
{
    auto& state = State();
    std::lock_guard lock(state.coldMutex);
    if (state.initialized) {
        return;
    }
    state.initialized = true;
    const char* output = std::getenv("FBZZ_FRAME_TIMELINE_OUTPUT");
    if (output && *output) {
        state.output = output;
    }
    state.eventCapacity = EventLimitFromEnvironment();
}

bool IsEnabled() { return State().enabled.load(std::memory_order_acquire); }

bool ArmFromEnvironment(uint64_t observerSession, size_t warmupFrames, size_t sampleFrames)
{
    InitializeFromEnvironment();
    auto& state = State();
    std::lock_guard lock(state.coldMutex);
    if (state.output.empty() || state.capture) {
        return false;
    }
    if (observerSession == 0) {
        return false;
    }
    const bool sampleCountInRange = sampleFrames != 0 && sampleFrames <= MAX_FRAMES;
    if (!sampleCountInRange || warmupFrames > MAX_FRAMES) {
        return false;
    }

    // 記録領域と時刻の換算に必要な周波数をそろえてから、他のスレッドに公開する。
    auto capture = std::make_unique<Capture>(observerSession, warmupFrames, sampleFrames, state.eventCapacity);
#if defined(_WIN32)
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        return false;
    }
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
    if (!IsEnabled()) {
        return;
    }
    auto& capture = *State().capture;
    if (capture.observerSession == observerSession) {
        capture.cancelled.store(true, std::memory_order_release);
    }
}

Context CurrentContext() { return IsEnabled() ? s_context : Context{}; }

ContextScope::ContextScope(Context context)
{
    if (!IsEnabled()) {
        return;
    }

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
    if (!IsEnabled()) {
        return 0;
    }
    auto& capture = *State().capture;
    if (context.captureFrameOrdinal == 0) {
        context = s_context;
    }
    if (!IsInCaptureWindow(capture, context) || capture.cancelled.load(std::memory_order_acquire)) {
        return 0;
    }
    const CaptureWriter writer(capture);
    if (!writer.Valid()) {
        return 0;
    }
    const uint64_t now = ReadQpc(capture);
    return !threadCpu && IsFineSpan(event) ? DeferFineSpan(capture, event, context, value, now)
        : BeginAt(capture, event, context, value, threadCpu, now);
}

void End(uint64_t ticket, uint64_t value)
{
    if (ticket == 0 || !IsEnabled()) {
        return;
    }
    auto& capture = *State().capture;
    const CaptureWriter writer(capture);
    if (!writer.Valid()) {
        return;
    }

    const uint64_t now = ReadQpc(capture);
    if ((ticket & DEFERRED_TICKET_BIT) != 0) {
        EndFineSpan(capture, ticket, value, now);
    } else {
        EndAt(capture, ticket, value, now);
    }
}

void Mark(Event event, Context context, uint64_t value)
{
    if (!IsEnabled()) {
        return;
    }
    auto& capture = *State().capture;
    if (context.captureFrameOrdinal == 0) {
        context = s_context;
    }
    if (!IsInCaptureWindow(capture, context) || capture.cancelled.load(std::memory_order_acquire)) {
        return;
    }
    const CaptureWriter writer(capture);
    if (writer.Valid()) {
        (void)AppendEvent(capture, event, Edge::INSTANT, context, 0, value, false, ReadQpc(capture));
    }
}

Scope::Scope(Event event, Context context, uint64_t value, bool threadCpu) : m_ticket(Begin(event, context, value, threadCpu)) {}
Scope::~Scope() { End(m_ticket); }

void BeginApplicationFrame(uint64_t applicationFrameSerial, uint64_t resourceFrameSerial, double profileStartMs)
{
    if (!IsEnabled()) {
        return;
    }
    auto& capture = *State().capture;
    if (capture.cancelled.load(std::memory_order_acquire)) {
        return;
    }

    // 選外のフレームも通過順に数え、スレッド内のイベントを正しいフレーム番号と関連付ける。
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

    // 前フレームの終了を取り逃しても、この開始時刻で開始から開始までの間隔を確定できる。
    if (capture.frameCount > 0) {
        auto& previous = capture.frames[capture.frameCount - 1];
        previous.times.nextBegin = now;
        previous.times.closed = true;
        const uint64_t previousSerial = previous.context.resourceFrameSerial;
        if (previousSerial == UINT64_MAX || resourceFrameSerial != previousSerial + 1) {
            capture.invalidBoundaries.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // 記録行に両方のフレーム番号と時刻を持たせ、プロファイラーや描画側の結果と照合できるようにする。
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

    // 終端フレームは前の区間を閉じるために使い、自身の CPU 計測区間は開かない。
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
    if (!IsEnabled() || s_mainFrameIndex == INVALID_INDEX || s_mainFrameTicket == 0) {
        return;
    }

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
    if (!IsEnabled()) {
        return false;
    }
    auto& state = State();
    auto& capture = *state.capture;
    // 出力処理を一つに絞り、受付を閉じてから進行中の書き込みと区間を確認する。
    if (capture.exported) {
        return false;
    }
    if (capture.closing.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    if (capture.activeWriters.load(std::memory_order_acquire) != 0
        || capture.activeScopes.load(std::memory_order_acquire) != 0) {
        capture.closing.store(false, std::memory_order_release);
        return false;
    }

    // すべての区間を確認した後に記録領域を読み出し、出力成功時に収集を無効にする。
    if (!detail::PublishCaptureFiles(capture, state.output)) {
        return false;
    }

    capture.exported = true;
    state.enabled.store(false, std::memory_order_release);
    return true;
}

} // namespace fbzz::profiler::frame_timeline
