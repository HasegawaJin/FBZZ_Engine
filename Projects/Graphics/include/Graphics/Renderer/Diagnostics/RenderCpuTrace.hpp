/// @file    RenderCpuTrace.hpp
/// @brief   Opt-in bounded CSV diagnostics for producer CPU phases and render-thread synchronization.
/// @author  Hasegawa Jin
/// @date    2026-10-13
#pragma once
#include <Graphics/Api.hpp>

#include <cstdint>

namespace fbzz::renderer::cpu_trace {

enum class Phase : uint8_t { OUTSIDE, SETUP, SKIN, EXTRACT, ENVIRONMENT_PIPELINE, GRAPH, POST_CLEANUP, VIEW };
enum class Source : uint8_t { UNKNOWN, BUFFER_GET, BATCH_STATIC, BATCH_DEFORMED, BATCH_FLUSH };
enum class WaitKind : uint8_t { UNSPECIFIED, SLOT, ACCEPTANCE, COMPLETION, DRAIN };
enum class Counter : uint8_t { SYNC, CPU_FRAME_WAIT, BUFFER_PRODUCER, BUFFER_WORKER, BUFFER_INVALID, BUFFER_DETACHED, EMPTY_SUBMIT, PACKETS, COMMANDS, SOURCE_OPERATIONS, COUNT };
enum class AcquireKind : uint8_t { EXECUTION_MUTEX, THREAD_MUTEX };
enum class TestCpuMode : uint8_t { NATIVE, UNAVAILABLE, REGRESSING };

/// @note Fixed upper bounds; exceeding any bound marks the capture incomplete rather than growing storage.
inline constexpr uint32_t MAX_SELECTED_FRAMES = 32, MAX_VIEWS = 256, MAX_PHASES = 2048;
inline constexpr uint32_t MAX_DOMAINS = 8192, MAX_PACKETS = 65536, MAX_THREADS = 16;
inline constexpr uint32_t MAX_API_ORIGINS = 64, MAX_API_ORIGIN_BYTES = 512, MAX_REENTRY = 8;

namespace detail {
struct Context {
    uint64_t threadId = 0, applicationFrame = 0, resourceFrame = 0, resourceInstance = 0, resourceEpoch = 0, sceneGeneration = 0;
    uint32_t token = 0, viewOrdinal = 0, parentViewOrdinal = 0, viewId = 0, outputId = 0, outputGeneration = 0, depth = 0;
    Phase phase = Phase::OUTSIDE;
    Source source = Source::UNKNOWN;
    WaitKind waitKind = WaitKind::UNSPECIFIED;
    uint16_t api = 0;
    bool worker = false;
};
/// @note CPU cycles remain raw execution counters; never convert them to elapsed time using an assumed frequency.
struct Stamp {
    uint64_t wall = 0, kernel = 0, user = 0, thread = 0, cycles = 0;
    bool cpuAvailable = false, cyclesAvailable = false;
};
} /// @note namespace detail

/// @note Initialize outside the hot loop. Missing FBZZ_RENDER_CPU_TRACE_OUTPUT leaves all capture branches disabled.
FBZZ_GRAPHICS_API void InitializeFromEnvironment();
[[nodiscard]] FBZZ_GRAPHICS_API bool IsEnabled();
[[nodiscard]] FBZZ_GRAPHICS_API bool HasSelectedFrame();
/// @note Register identity includes a monotonically issued lifetime ID, not only the object's address.
FBZZ_GRAPHICS_API void RegisterThread(const void* thread);
FBZZ_GRAPHICS_API void UnregisterThread(const void* thread);
FBZZ_GRAPHICS_API void ThreadStopped(const void* thread);
FBZZ_GRAPHICS_API void BeginFrame(const void* thread, uint64_t applicationFrame);
FBZZ_GRAPHICS_API void Count(Counter counter, uint64_t amount = 1);
/// @note The packet's immutable origin describes submission, not the exclusive origin of all queued commands.
FBZZ_GRAPHICS_API void BindPacket(const void* thread, uint64_t serial, uint64_t commands);
FBZZ_GRAPHICS_API void PacketAccepted(const void* thread, uint64_t serial, bool accepted);
/// @pre All registered worker threads have stopped through the existing drain/Stop path.
/// @note No additional RHI wait is introduced; serialization and file I/O occur only here.
[[nodiscard]] FBZZ_GRAPHICS_API bool WriteCsvAfterStop();

class SourceScope final {
public:
    FBZZ_GRAPHICS_API SourceScope(Source source, const char* api = nullptr, bool preserveSource = false);
    FBZZ_GRAPHICS_API ~SourceScope();
    SourceScope(const SourceScope&) = delete;
    SourceScope& operator=(const SourceScope&) = delete;
private:
    Source m_previousSource = Source::UNKNOWN;
    uint16_t m_previousApi = 0;
    bool m_active = false;
};

class WaitKindScope final {
public:
    FBZZ_GRAPHICS_API explicit WaitKindScope(WaitKind kind);
    FBZZ_GRAPHICS_API ~WaitKindScope();
    WaitKindScope(const WaitKindScope&) = delete;
    WaitKindScope& operator=(const WaitKindScope&) = delete;
private:
    WaitKind m_previous = WaitKind::UNSPECIFIED;
    bool m_active = false;
};

class ViewScope final {
public:
    FBZZ_GRAPHICS_API ViewScope(uint64_t resourceInstance, uint64_t resourceEpoch, uint64_t resourceFrame,
        uint64_t sceneGeneration, uint32_t outputId, uint32_t outputGeneration);
    FBZZ_GRAPHICS_API ~ViewScope();
    FBZZ_GRAPHICS_API void SetViewId(uint32_t viewId);
    FBZZ_GRAPHICS_API void SetPhase(Phase phase);
    ViewScope(const ViewScope&) = delete;
    ViewScope& operator=(const ViewScope&) = delete;
private:
    detail::Context m_previous{};
    detail::Stamp m_viewStart{}, m_phaseStart{};
    uint32_t m_viewRow = UINT32_MAX, m_phaseRow = UINT32_MAX;
    bool m_active = false;
};

/// @note Fast-ready waits count without reading a wall or OS CPU clock.
/// @note Leaf elapsed subtracts only directly nested waits' inclusive elapsed; overflow makes leaf unavailable.
class WaitScope final {
public:
    FBZZ_GRAPHICS_API explicit WaitScope(bool initiallyNotReady);
    FBZZ_GRAPHICS_API ~WaitScope();
    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;
private:
    friend class PumpScope;
    WaitScope* m_parent = nullptr;
    uint64_t m_start = 0, m_children = 0;
    uint32_t m_cell = UINT32_MAX, m_depth = 0;
    bool m_timed = false, m_leafAvailable = true;
};

/// @note Pump elapsed overlaps nested waits and must not be added to wait elapsed.
class PumpScope final {
public:
    FBZZ_GRAPHICS_API PumpScope();
    FBZZ_GRAPHICS_API ~PumpScope();
    PumpScope(const PumpScope&) = delete;
    PumpScope& operator=(const PumpScope&) = delete;
private:
    uint64_t m_start = 0;
    uint32_t m_cell = UINT32_MAX;
};

/// @note Lock acquisition elapsed is not proof that the mutex was contended.
class AcquireScope final {
public:
    FBZZ_GRAPHICS_API explicit AcquireScope(AcquireKind kind);
    FBZZ_GRAPHICS_API ~AcquireScope();
    FBZZ_GRAPHICS_API void Acquired();
    AcquireScope(const AcquireScope&) = delete;
    AcquireScope& operator=(const AcquireScope&) = delete;
private:
    uint64_t m_start = 0;
    uint32_t m_cell = UINT32_MAX;
    AcquireKind m_kind = AcquireKind::EXECUTION_MUTEX;
};

class PacketScope final {
public:
    FBZZ_GRAPHICS_API PacketScope(const void* thread, uint64_t serial);
    FBZZ_GRAPHICS_API ~PacketScope();
    FBZZ_GRAPHICS_API void BeginDestruction();
    FBZZ_GRAPHICS_API void FinishedDestruction();
    PacketScope(const PacketScope&) = delete;
    PacketScope& operator=(const PacketScope&) = delete;
private:
    detail::Context m_previous{};
    detail::Stamp m_start{};
    uint32_t m_row = UINT32_MAX;
    bool m_destroying = false, m_finished = false;
};

/// @note Cold test/snapshot API; never invoked by the render loop.
struct Summary {
    uint64_t selectedFrames = 0, views = 0, phases = 0, waitCalls = 0, notReady = 0;
    uint64_t waitInclusiveNs = 0, waitLeafNs = 0, pumpCalls = 0, cpuQueries = 0, wallQueries = 0, cycleQueries = 0;
    uint64_t packets = 0, completedPackets = 0, dropped = 0, bufferProducer = 0, bufferWorker = 0;
    uint64_t emptySubmits = 0, sourceStaticCalls = 0, sourceDeformedCalls = 0;
    bool complete = true, leafAvailable = true, cpuAvailable = true, cyclesAvailable = true;
};
[[nodiscard]] FBZZ_GRAPHICS_API Summary SnapshotSummary();
struct PacketInfo {
    uint64_t threadLifetimeId = 0, applicationFrame = 0, resourceFrame = 0, serial = 0;
    uint32_t token = 0, viewOrdinal = 0, viewId = 0;
    bool accepted = false, complete = false;
};
[[nodiscard]] FBZZ_GRAPHICS_API PacketInfo SnapshotPacket(uint32_t index);
/// @pre No trace scope or packet is active. Existing idle workers may be registered again at the next BeginFrame.
[[nodiscard]] FBZZ_GRAPHICS_API bool ConfigureForTesting(uint32_t warmup = 0, uint32_t stride = 1,
    uint32_t samples = 1, uint32_t packetLimit = MAX_PACKETS, TestCpuMode cpuMode = TestCpuMode::NATIVE,
    const char* outputPath = nullptr);
/// @pre All trace scopes/packets have finished; fixture shutdown precedes disabling/resetting capture state.
[[nodiscard]] FBZZ_GRAPHICS_API bool DisableForTesting();

} /// @note namespace fbzz::renderer::cpu_trace
