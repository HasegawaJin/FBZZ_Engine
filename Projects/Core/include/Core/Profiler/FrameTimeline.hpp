/// @file    FrameTimeline.hpp
/// @brief   Callback-free opt-in frame, packet and wait chronology with bounded storage.
/// @author  Hasegawa Jin
/// @date    2026-10-16
#pragma once
#include <Core/Api.hpp>

#include <cstddef>
#include <cstdint>

namespace fbzz::profiler::frame_timeline {

enum class Event : uint16_t {
    MAIN_FRAME, TIME_TICK, INPUT, WINDOW_EVENTS, INPUT_CALLBACKS, INPUT_ACTIONS,
    ASSET_PUMP, MODULE_UPDATE, MODULE_LATE_UPDATE, AUDIO_UPDATE, MODULE_RENDER,
    FRAME_TAIL, EDITOR_UPDATE, SIMULATION, VIEW, SKINNING, EXTRACT, ENVIRONMENT,
    GRAPH, POST_CLEANUP, WORKER_FRAME, PACKET, PACKET_EXECUTE, PACKET_DESTROY,
    PACKET_SUBMIT, PACKET_ACCEPT, WAIT_SLOT, WAIT_ACCEPTANCE, WAIT_COMPLETION,
    WAIT_DRAIN, WAIT_OTHER, WAIT_PUMP, EXECUTION_MUTEX, THREAD_MUTEX, FENCE_WAIT,
    SYNCHRONOUS_FENCE_WAIT, PRESENT, QUEUE_SUBMIT, COUNT
};

/// @note IDs are copied provenance only; no scene, resource or renderer lifetime is retained.
struct Context {
    uint64_t captureFrameOrdinal = 0;
    uint64_t applicationFrameSerial = 0;
    uint64_t resourceFrameSerial = 0;
    uint64_t submissionSerial = 0;
    uint64_t targetSerial = 0;
    uint64_t objectId = 0;
    uint32_t viewId = 0;
    uint32_t detail = 0;
};

/// @note Initialize outside the frame loop; an absent FBZZ_FRAME_TIMELINE_OUTPUT keeps clocks and allocations disabled.
FBZZ_CORE_API void InitializeFromEnvironment();
[[nodiscard]] FBZZ_CORE_API bool IsEnabled();
/// @note One opt-in capture per process; reserve storage before the next passive observer boundary.
/// @note Capture includes the final warmup frame, sample frames and a closing next-Begin anchor without changing passive timing semantics.
[[nodiscard]] FBZZ_CORE_API bool ArmFromEnvironment(uint64_t observerSession, size_t warmupFrames, size_t sampleFrames);
FBZZ_CORE_API void CancelCapture(uint64_t observerSession);
/// @note Resource and application serials are distinct domains; profileStartMs is the existing passive clock anchor.
FBZZ_CORE_API void BeginApplicationFrame(uint64_t applicationFrameSerial, uint64_t resourceFrameSerial, double profileStartMs);
FBZZ_CORE_API void EndApplicationFrame(double profileEndMs);

[[nodiscard]] FBZZ_CORE_API Context CurrentContext();

class ContextScope final {
public:
    FBZZ_CORE_API explicit ContextScope(Context context);
    FBZZ_CORE_API ~ContextScope();
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
private:
    Context m_previous{};
    bool m_active = false;
};

/// @return Zero when disabled, outside the selected window or capacity is exhausted.
/// @note Empty context uses the current thread's copied context. Explicit contexts should start from CurrentContext().
/// @note Read thread CPU counters only at coarse frame boundaries; raw cycles are never converted to elapsed time.
[[nodiscard]] FBZZ_CORE_API uint64_t Begin(Event event, Context context = {}, uint64_t value = 0, bool threadCpu = false);
/// @pre A nonzero ticket is ended exactly once on the same OS thread that began it.
FBZZ_CORE_API void End(uint64_t ticket, uint64_t value = 0);
FBZZ_CORE_API void Mark(Event event, Context context = {}, uint64_t value = 0);

class Scope final {
public:
    FBZZ_CORE_API explicit Scope(Event event, Context context = {}, uint64_t value = 0, bool threadCpu = false);
    FBZZ_CORE_API ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    uint64_t m_ticket = 0;
};

/// @pre All render workers and timeline scopes have stopped through their existing shutdown path.
/// @note Performs no RHI wait or callbacks; CSV serialization and file writes occur only here.
[[nodiscard]] FBZZ_CORE_API bool WriteAfterStop();

} /// @note namespace fbzz::profiler::frame_timeline
