/// @file    PassiveFrameElapsedCapture.hpp
/// @brief   Private scalar capture for fresh application boundaries with both collectors stopped.
/// @author  Hasegawa Jin
/// @date    2026-10-13
#pragma once
#include <ApplicationFrameElapsedObserver.hpp>
#include <Editor/Profiler/ProfilerHistory.hpp>
#include <string>
#include <vector>

namespace fbzz::editor {

inline constexpr const char* PASSIVE_FRAME_ELAPSED_MODE = "application-frame-elapsed/passive-v1";
enum class PassiveFrameElapsedFailure { NONE, MODE_CHANGED, SESSION_CHANGED, FRAME_ORDER_CHANGED, PLAY_CHANGED };
struct PassiveFrameElapsedConditions {
    std::string scenario, camera, configuration, runtimeHash, scenarioHash;
    bool operator==(const PassiveFrameElapsedConditions&) const = default;
};
struct PassiveFrameElapsedStatus {
    PerformanceBenchmarkState state = PerformanceBenchmarkState::IDLE;
    PassiveFrameElapsedFailure failure = PassiveFrameElapsedFailure::NONE;
    uint64_t observerSessionId = 0, missingFrameCount = 0, missingPublicationCount = 0;
    size_t warmupFrames = 0, warmedFrames = 0, sampleFrames = 0, capturedFrames = 0;
    size_t incompleteFrames = 0, missingRenderingMetadataFrames = 0;
    size_t missingWarmupRenderingMetadataFrames = 0;
};
struct PassiveFrameElapsedRow {
    profiler::detail::ApplicationFrameElapsedSnapshot elapsed;
    renderer::GpuProfilerViewMetadata rendering;
    int drawCalls = 0, triangleCount = 0;
    bool renderingAvailable = false;
};
/// @note Availability belongs to the passive report contract and is checked independently of count.
struct PassiveFrameElapsedDistribution : PerformanceBenchmarkDistribution {
    bool available = false;
};
struct PassiveFrameElapsedReport {
    std::string label;
    PassiveFrameElapsedConditions conditions;
    PassiveFrameElapsedStatus status;
    PassiveFrameElapsedDistribution cpu, wall;
    renderer::GpuProfilerViewMetadata rendering;
    bool renderingMetadataAvailable = false, renderingConditionsChanged = false;
    bool failureFrameAvailable = false;
    profiler::detail::ApplicationFrameElapsedModeWitness baselineMode;
    profiler::detail::ApplicationFrameElapsedSnapshot failureFrame;
};

/// @note Start reserves bounded scalar storage; Feed copies no scope/descriptor/vector snapshot.
class PassiveFrameElapsedCapture {
public:
    [[nodiscard]] bool CanStart(const std::string& label, size_t warmup, size_t samples,
        const PassiveFrameElapsedConditions& conditions) const;
    bool Start(std::string label, size_t warmup, size_t samples, uint64_t observerSession,
        profiler::detail::ApplicationFrameElapsedModeWitness baseline, PassiveFrameElapsedConditions conditions = {});
    void Feed(const profiler::detail::ApplicationFrameElapsedSnapshot& elapsed,
        const renderer::RenderDebugOverlay::Snapshot& rendering, bool playing = true);
    bool Cancel();
    [[nodiscard]] bool IsActive() const;
    [[nodiscard]] const PassiveFrameElapsedReport& Report() const { return m_report; }
    [[nodiscard]] const PassiveFrameElapsedStatus& Status() const { return m_report.status; }
    [[nodiscard]] const std::vector<PassiveFrameElapsedRow>& Rows() const { return m_rows; }
private:
    void Fail(PassiveFrameElapsedFailure failure, const profiler::detail::ApplicationFrameElapsedSnapshot& frame);
    PassiveFrameElapsedReport m_report;
    profiler::detail::ApplicationFrameElapsedModeWitness m_baseline;
    std::vector<PassiveFrameElapsedRow> m_rows;
    std::vector<double> m_cpuTimings, m_wallTimings;
    uint64_t m_lastSerial = 0, m_lastOrdinal = 0;
    bool m_hasFrame = false;
};
PassiveFrameElapsedCapture& GetPassiveFrameElapsedCapture();
bool StartPassiveFrameElapsedCapture(EditorContext& ctx, std::string label, size_t warmup, size_t samples,
    PassiveFrameElapsedConditions conditions = {});
bool CancelPassiveFrameElapsedCapture();
bool SetPassiveFrameElapsedReference();
const PassiveFrameElapsedReport* GetPassiveFrameElapsedReference();
/// @note Strongly typed passive reports cannot be substituted for a legacy native benchmark report.
[[nodiscard]] bool IsPassiveFrameElapsedEligible(const PassiveFrameElapsedReport& report);
[[nodiscard]] bool ArePassiveFrameElapsedComparable(const PassiveFrameElapsedReport& reference,
    const PassiveFrameElapsedReport& candidate);

} /// @note namespace fbzz::editor
