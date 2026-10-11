/// @file    ProfilerHistory.hpp
/// @brief   所有 snapshot の履歴、画面選択とスクリプト集計。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#pragma once
#include <Core/Profiler/Profiler.hpp>
#include <Engine/Profiler/ScriptProfiler.hpp>
#include <Engine/Renderer/RenderDebugOverlay.hpp>
#include <Core/Memory/MemoryTracker.hpp>
#include <array>
#include <deque>
#include <memory>
#include <string>
#include <vector>
namespace fbzz::editor {
struct EditorContext;
struct PerformanceCapture : profiler::PerformanceSnapshot {
    renderer::RenderDebugOverlay::Snapshot rendering;
};
template<class Snapshot> struct ProfilerCaptureHistory {
    std::deque<std::shared_ptr<const Snapshot>> frames;
    std::shared_ptr<const Snapshot> selected;
    std::shared_ptr<const Snapshot> spike;
    uint64_t lastSession = 0;
    uint64_t lastSerial = 0;
    uint64_t historyEvictedFrameCount = 0;
    bool frozen = false;
    bool budgetBlocked = false;
};
using PerformanceHistory = ProfilerCaptureHistory<PerformanceCapture>;
using ScriptHistory = ProfilerCaptureHistory<scene::ScriptProfileSnapshot>;
struct MemoryProfileTag {
    std::string name;
    core::MemoryStats stats;
};
struct MemoryProfileSnapshot : profiler::ProfileSnapshot {
    std::vector<MemoryProfileTag> tags;
    core::MemoryStats frameAllocator;
};
using MemoryHistory = ProfilerCaptureHistory<MemoryProfileSnapshot>;
enum class PerformanceBenchmarkState { IDLE, WARMUP, SAMPLING, COMPLETE, FAILED, CANCELLED };
enum class PerformanceBenchmarkFailure { NONE, SESSION_CHANGED, FRAME_ORDER_CHANGED, RECORDING_STOPPED };
enum class PhysicsBenchmarkScope : uint8_t {
    WORLD_STEP, SYSTEM, SYNC_RIGID_BODIES, SYNC_COLLIDERS, SUBMIT_SCENE_STATE,
    DISPATCH_COLLISION_EVENTS, WRITE_BACK_TRANSFORMS, COUNT
};
constexpr size_t PHYSICS_BENCHMARK_SCOPE_COUNT = static_cast<size_t>(PhysicsBenchmarkScope::COUNT);
const char* PhysicsBenchmarkScopeName(PhysicsBenchmarkScope scope);
struct PhysicsBenchmarkScopeSample {
    uint32_t calls = 0;
    double totalMs = 0.0;
};
struct PhysicsBenchmarkRow {
    std::array<PhysicsBenchmarkScopeSample, PHYSICS_BENCHMARK_SCOPE_COUNT> scopes{};
    uint32_t stepCount = 0;
    uint32_t faultedScopeCount = 0;
    uint32_t abortedScopeCount = 0;
    uint32_t invalidScopeCount = 0;
    bool complete = true;
};
struct PerformanceBenchmarkConditions {
    std::string scenario;
    std::string camera;
    /// @note 呼出元が固定した dt/seed/quality/pacing/build 等を宣言する。Profiler は実行条件を変更しない。
    std::string configuration;
    bool operator==(const PerformanceBenchmarkConditions&) const = default;
};
struct PerformanceBenchmarkRow {
    uint64_t captureSessionId = 0;
    uint64_t applicationFrameSerial = 0;
    uint32_t droppedSampleCount = 0;
    uint32_t filteredSampleCount = 0;
    uint32_t recordedSampleCount = 0;
    uint32_t sampleCapacity = 8192;
    double cpuFrameElapsedMs = 0.0;
    double wallFrameIntervalMs = 0.0;
    int drawCalls = 0;
    int triangleCount = 0;
    bool complete = true;
    bool cpuFrameElapsedAvailable = false;
    bool wallFrameIntervalAvailable = false;
    bool renderingAvailable = false;
    PhysicsBenchmarkRow physics;
};
struct PerformanceBenchmarkDistribution {
    size_t count = 0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double maxMs = 0.0;
};
struct PhysicsBenchmarkScopeSummary {
    PerformanceBenchmarkDistribution frameMs;
    /// @note step がある frame の totalMs / stepCount の分布であり、個別 step の分布ではない。
    PerformanceBenchmarkDistribution msPerStep;
    uint64_t calls = 0;
    double totalMs = 0.0;
};
struct PhysicsBenchmarkSummary {
    std::array<PhysicsBenchmarkScopeSummary, PHYSICS_BENCHMARK_SCOPE_COUNT> scopes{};
    uint64_t observedStepCount = 0;
    uint64_t stepCount = 0;
    uint64_t validFrames = 0;
    uint64_t excludedFrames = 0;
    uint64_t zeroStepFrames = 0;
    uint64_t multiStepFrames = 0;
    uint64_t faultedScopeCount = 0;
    uint64_t abortedScopeCount = 0;
    uint64_t invalidScopeCount = 0;
};
struct PerformanceBenchmarkStatus {
    PerformanceBenchmarkState state = PerformanceBenchmarkState::IDLE;
    PerformanceBenchmarkFailure failure = PerformanceBenchmarkFailure::NONE;
    size_t warmupFrames = 0;
    size_t warmedFrames = 0;
    size_t sampleFrames = 0;
    size_t capturedFrames = 0;
    size_t incompleteFrames = 0;
    uint64_t droppedSampleCount = 0;
    uint64_t missingFrameCount = 0;
    uint64_t captureSessionId = 0;
};
struct PerformanceBenchmarkReport {
    std::string label;
    PerformanceBenchmarkConditions conditions;
    profiler::PerformanceCaptureFilter captureFilter;
    PerformanceBenchmarkStatus status;
    PerformanceBenchmarkDistribution cpu;
    PerformanceBenchmarkDistribution wall;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t sceneGeneration = 0;
    bool renderingMetadataAvailable = false;
    bool renderingConditionsChanged = false;
};
struct PerformanceBenchmarkComparison {
    bool comparable = false;
    double cpuP50DeltaMs = 0.0;
    double cpuP95DeltaMs = 0.0;
    std::vector<std::string> warnings;
};
/// @note 欠落・未完了・条件未宣言・異なる収集範囲を警告し、その場合は差分を公開しない。
PerformanceBenchmarkComparison ComparePerformanceBenchmarks(const PerformanceBenchmarkReport& reference,
    const PerformanceBenchmarkReport& candidate);
/// @note main thread の完成済み frame 境界を採る。C# callback の測定区間との一致は呼出元が調整する。
/// @note Start で scalar 領域を予約し、Feed は sample/descriptor をコピーせず追加確保もしない。
class PerformanceBenchmarkCapture {
public:
    bool Start(std::string label, size_t warmupFrames, size_t sampleFrames, const profiler::PerformanceSnapshot& baseline,
        PerformanceBenchmarkConditions conditions = {});
    [[nodiscard]] PerformanceBenchmarkReport Report() const;
    void Feed(const profiler::PerformanceSnapshot& snapshot, const renderer::RenderDebugOverlay::Snapshot& rendering,
        bool recordingRequested = true);
    bool Cancel();
    [[nodiscard]] bool IsActive() const;
    [[nodiscard]] const std::string& Label() const { return m_label; }
    [[nodiscard]] const PerformanceBenchmarkStatus& Status() const { return m_status; }
    [[nodiscard]] const std::vector<PerformanceBenchmarkRow>& Rows() const { return m_rows; }
    /// @note nearest-rank percentile は有限かつ非負の available 値だけで計算する。標本欠落は Status と各 row に残す。
    [[nodiscard]] const PerformanceBenchmarkDistribution& CpuSummary() const { return m_cpuSummary; }
    [[nodiscard]] const PerformanceBenchmarkDistribution& WallSummary() const { return m_wallSummary; }
    /// @note Physics は完全な frame だけ集計し、零 step の frame は frameMs のみに含める。
    [[nodiscard]] const PhysicsBenchmarkSummary& PhysicsSummary() const { return m_physicsSummary; }
private:
    std::string m_label;
    PerformanceBenchmarkConditions m_conditions;
    profiler::PerformanceCaptureFilter m_captureFilter;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint64_t m_sceneGeneration = 0;
    bool m_renderingMetadataAvailable = false;
    bool m_renderingConditionsChanged = false;
    PerformanceBenchmarkStatus m_status;
    std::vector<PerformanceBenchmarkRow> m_rows;
    std::vector<double> m_cpuTimings;
    std::vector<double> m_wallTimings;
    PerformanceBenchmarkDistribution m_cpuSummary;
    PerformanceBenchmarkDistribution m_wallSummary;
    std::array<std::vector<double>, PHYSICS_BENCHMARK_SCOPE_COUNT> m_physicsFrameTimings;
    std::array<std::vector<double>, PHYSICS_BENCHMARK_SCOPE_COUNT> m_physicsPerStepTimings;
    PhysicsBenchmarkSummary m_physicsSummary;
    uint64_t m_lastSession = 0;
    uint64_t m_lastSerial = 0;
    bool m_hasBaseline = false;
    bool m_hasAcceptedFrame = false;
};
PerformanceBenchmarkCapture& GetPerformanceBenchmarkCapture();
/// @note recording が停止中なら測定の間だけ有効化し、終了時に元の停止状態へ戻す。
bool StartPerformanceBenchmark(std::string label, size_t warmupFrames, size_t sampleFrames,
    PerformanceBenchmarkConditions conditions = {});
bool SetPerformanceBenchmarkReference();
const PerformanceBenchmarkReport* GetPerformanceBenchmarkReference();
bool CancelPerformanceBenchmark();
enum class ScriptProfileGroup { TYPE, CALLBACK_KIND, INSTANCE };
struct ScriptProfileFilter {
    std::string name;
    std::string type;
    std::string instance;
    std::string callback;
    std::string mode;
    uint64_t sceneGeneration = 0;
};
struct ScriptProfileRow {
    scene::ScriptProfileDescriptor descriptor;
    scene::ScriptProfileContext context;
    uint64_t calls = 0;
    uint64_t faults = 0;
    uint64_t completedCalls = 0;
    uint64_t validFrames = 0;
    uint64_t excludedFrames = 0;
    double inclusiveMs = 0.0;
    double selfMs = 0.0;
    double maxCallMs = 0.0;
    double peakFrameMs = 0.0;
    double faultElapsedMs = 0.0;
    double avgPerCallMs = 0.0;
    double avgPerFrameMs = 0.0;
    double share = 0.0;
    bool selfAvailable = true;
    bool shareAvailable = true;
};
PerformanceHistory& GetPerformanceHistory();
ScriptHistory& GetScriptHistory();
MemoryHistory& GetMemoryHistory();
bool IsMemoryHistoryRecording();
void SetMemoryHistoryRecording(bool recording);
void ClearMemoryHistory();
/// @note 各 session/serial を一度だけ採る。選択中の値と DLL 内文字列を共有しない。
void TickProfilerHistory(EditorContext& ctx);
void ResetProfilerHistory();
void ClearPerformanceHistory();
void ClearScriptHistory();
std::vector<ScriptProfileRow> AggregateScriptProfiles(
    const std::vector<const scene::ScriptProfileSnapshot*>& frames,
    ScriptProfileGroup group, const ScriptProfileFilter& filter = {});
bool MatchesScriptProfile(const scene::ScriptProfileDescriptor& descriptor,
                          const scene::ScriptProfileContext& context,
                          const ScriptProfileFilter& filter);
const scene::ScriptProfileDescriptor* FindScriptDescriptor(const scene::ScriptProfileSnapshot& snapshot, uint64_t key);
const scene::ScriptProfileContext* FindScriptContext(const scene::ScriptProfileSnapshot& snapshot, uint64_t key);
}
