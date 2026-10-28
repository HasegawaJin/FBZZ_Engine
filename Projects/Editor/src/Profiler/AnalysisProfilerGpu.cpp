/// @file    AnalysisProfilerGpu.cpp
/// @brief   Preserve GPU interval sums, missing measurements and frame provenance for diagnostic displays.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include <Profiler/AnalysisProfilerGpu.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace fbzz::editor::detail {

bool IsGpuPassMeasured(const renderer::GpuProfilerSnapshot& snapshot, const renderer::GpuPassProfile& pass)
{
    return snapshot.supported && snapshot.available && pass.available && std::isfinite(pass.gpuMs) && pass.gpuMs >= 0.0
        && pass.physicalFrameSerial == snapshot.physicalFrameSerial && pass.deviceEpoch == snapshot.deviceEpoch;
}

GpuDisplaySummary SummarizeGpuDisplay(const renderer::GpuProfilerSnapshot& snapshot, uint64_t applicationSerial)
{
    GpuDisplaySummary result;
    result.totalAvailable = snapshot.supported && snapshot.available && snapshot.totalGpuTimeAvailable
        && std::isfinite(snapshot.totalGpuMs) && snapshot.totalGpuMs >= 0.0;
    if (result.totalAvailable) result.totalMs = snapshot.totalGpuMs;
    result.partial = !snapshot.complete || snapshot.droppedPassCount != 0;
    bool ageMissing = false;
    for (const auto& pass : snapshot.passes) {
        if (!IsGpuPassMeasured(snapshot, pass)) { ++result.unavailableIntervals; continue; }
        result.passSumMs += pass.gpuMs;
        ++result.measuredIntervals;
        const uint64_t source = pass.metadata.applicationFrameSerial;
        if (source == 0 || applicationSerial == 0 || source > applicationSerial) { ageMissing = true; continue; }
        const uint64_t age = applicationSerial - source;
        result.youngestAge = result.ageAvailable ? (std::min)(result.youngestAge, age) : age;
        result.oldestAge = (std::max)(result.oldestAge, age);
        result.ageAvailable = true;
    }
    result.passSumAvailable = result.measuredIntervals != 0 && std::isfinite(result.passSumMs);
    result.partial = result.partial || result.unavailableIntervals != 0 || !std::isfinite(result.passSumMs);
    result.ageAvailable = result.ageAvailable && !ageMissing;
    return result;
}

std::vector<GpuPassGroup> BuildGpuPassGroups(const renderer::GpuProfilerSnapshot& snapshot)
{
    std::vector<GpuPassGroup> groups;
    groups.reserve(snapshot.passes.size());
    for (const auto& pass : snapshot.passes) {
        auto found = std::find_if(groups.begin(), groups.end(), [&](const GpuPassGroup& group) {
            const auto& source = *group.source;
            return source.name == pass.name && source.metadata == pass.metadata
                && source.physicalFrameSerial == pass.physicalFrameSerial && source.deviceEpoch == pass.deviceEpoch;
        });
        if (found == groups.end()) { groups.push_back({&pass}); found = groups.end() - 1; }
        ++found->intervalCount;
        if (IsGpuPassMeasured(snapshot, pass)) { found->gpuMs += pass.gpuMs; ++found->measuredIntervals; }
        found->available = found->measuredIntervals != 0 && std::isfinite(found->gpuMs);
        found->partial = found->measuredIntervals != found->intervalCount || !std::isfinite(found->gpuMs);
    }
    return groups;
}

std::string GpuPassSeriesKey(const renderer::GpuPassProfile& pass)
{
    const auto& view = pass.metadata;
    char context[256];
    std::snprintf(context, sizeof(context), "/%llu/%llu/%llu/%llu/%llu/%u/%u/%ux%u",
        static_cast<unsigned long long>(pass.deviceEpoch), static_cast<unsigned long long>(view.viewId),
        static_cast<unsigned long long>(view.sceneGeneration), static_cast<unsigned long long>(view.planGeneration),
        static_cast<unsigned long long>(view.resourceEpoch), view.outputId, view.outputGeneration, view.width, view.height);
    return std::to_string(pass.name.size()) + ":" + pass.name + context;
}

std::string GpuPassRowKey(const renderer::GpuPassProfile& pass)
{
    return GpuPassSeriesKey(pass) + "/source/" + std::to_string(pass.metadata.applicationFrameSerial)
        + "/" + std::to_string(pass.physicalFrameSerial);
}

std::vector<GpuDisplayRow> BuildGpuDisplayRows(const renderer::GpuProfilerSnapshot& snapshot)
{
    if (!snapshot.supported || !snapshot.available) return {};
    auto groups = BuildGpuPassGroups(snapshot);
    std::stable_sort(groups.begin(), groups.end(), [](const GpuPassGroup& a, const GpuPassGroup& b) {
        if (a.source->metadata.applicationFrameSerial != b.source->metadata.applicationFrameSerial)
            return a.source->metadata.applicationFrameSerial > b.source->metadata.applicationFrameSerial;
        return a.source->physicalFrameSerial > b.source->physicalFrameSerial;
    });
    std::vector<GpuDisplayRow> rows;
    rows.reserve(groups.size());
    for (const auto& group : groups) {
        const auto series = GpuPassSeriesKey(*group.source);
        const size_t slot = static_cast<size_t>(std::count_if(rows.begin(), rows.end(), [&](const auto& row) {
            return row.seriesKey == series;
        }));
        rows.push_back({series + "/slot/" + std::to_string(slot), series, *group.source, group.intervalCount,
            group.measuredIntervals, group.gpuMs, group.available, group.partial});
    }
    return rows;
}

std::vector<GpuHistoryDisplaySample> BuildGpuObservationPlot(const std::vector<GpuHistoryDisplaySample>& samples,
    const std::deque<uint64_t>& observedFrames)
{
    std::vector<GpuHistoryDisplaySample> result;
    result.reserve(observedFrames.size() + samples.size());
    for (const uint64_t frame : observedFrames) {
        const size_t begin = result.size();
        for (const auto& sample : samples) if (sample.observedSerial == frame) result.push_back(sample);
        if (begin == result.size()) {
            GpuHistoryDisplaySample gap;
            gap.observedSerial = frame; gap.ms = (std::numeric_limits<float>::quiet_NaN)();
            result.push_back(gap);
        }
    }
    return result;
}

} /// @note namespace fbzz::editor::detail
