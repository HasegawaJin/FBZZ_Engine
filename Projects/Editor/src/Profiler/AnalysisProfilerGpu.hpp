/// @file    AnalysisProfilerGpu.hpp
/// @brief   Shared GPU display summaries and explicit same-name interval aggregation.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once

#include <Graphics/Renderer/GpuProfiler.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace fbzz::editor::detail {

struct GpuDisplaySummary {
    bool passSumAvailable = false;
    bool totalAvailable = false;
    bool partial = false;
    bool ageAvailable = false;
    double passSumMs = 0.0;
    double totalMs = 0.0;
    size_t measuredIntervals = 0;
    size_t unavailableIntervals = 0;
    uint64_t youngestAge = 0;
    uint64_t oldestAge = 0;
};

/// @note A group is a sum of observed intervals with identical name and complete source metadata, never a GPU frame total.
/// @note source references the caller-owned snapshot; keep that snapshot alive while consuming the groups.
struct GpuPassGroup {
    const renderer::GpuPassProfile* source = nullptr;
    size_t intervalCount = 0;
    size_t measuredIntervals = 0;
    double gpuMs = 0.0;
    bool available = false;
    bool partial = false;
};

[[nodiscard]] bool IsGpuPassMeasured(const renderer::GpuProfilerSnapshot& snapshot, const renderer::GpuPassProfile& pass);
[[nodiscard]] GpuDisplaySummary SummarizeGpuDisplay(const renderer::GpuProfilerSnapshot& snapshot, uint64_t applicationSerial);
[[nodiscard]] std::vector<GpuPassGroup> BuildGpuPassGroups(const renderer::GpuProfilerSnapshot& snapshot);
/// @note History identity excludes source frame serials; the row identity includes them for distinct observations in one snapshot.
[[nodiscard]] std::string GpuPassSeriesKey(const renderer::GpuPassProfile& pass);
[[nodiscard]] std::string GpuPassRowKey(const renderer::GpuPassProfile& pass);

/// @note Observation keys retain frame provenance; display keys use stable source slots within each semantic series.
struct GpuDisplayRow {
    std::string key;
    std::string seriesKey;
    renderer::GpuPassProfile source;
    size_t intervalCount = 0;
    size_t measuredIntervals = 0;
    double gpuMs = 0.0;
    bool available = false;
    bool partial = false;
    [[nodiscard]] size_t DynamicBytes() const { return key.capacity() + seriesKey.capacity() + source.name.capacity() + 3; }
};
[[nodiscard]] std::vector<GpuDisplayRow> BuildGpuDisplayRows(const renderer::GpuProfilerSnapshot& snapshot);

struct GpuHistoryDisplaySample {
    uint64_t observedSerial = 0;
    uint64_t applicationSerial = 0;
    uint64_t physicalSerial = 0;
    float ms = 0.0f;
    size_t intervalCount = 0;
    size_t measuredIntervals = 0;
    bool partial = false;
};
/// @note Every observed capture without a new measured source yields a NaN gap; repeated asynchronous results are not remeasured.
[[nodiscard]] std::vector<GpuHistoryDisplaySample> BuildGpuObservationPlot(
    const std::vector<GpuHistoryDisplaySample>& samples, const std::deque<uint64_t>& observedFrames);

} /// @note namespace fbzz::editor::detail
