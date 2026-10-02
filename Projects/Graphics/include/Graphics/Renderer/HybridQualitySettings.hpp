/// @file    HybridQualitySettings.hpp
/// @brief   Explicit Hybrid workload limits and non-binding frame budget targets.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#pragma once
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace fbzz::renderer {

enum class HybridQualityPreset : uint8_t { CUSTOM, LOW, BALANCED, HIGH };

/// @note Targets are not measurements or automatic quality changes. Shared preparation and overlapping queues must be reported separately.
/// @see Docs/design/RayTracing.md Frame budgets, independent effect quality and bounded approximation.
struct HybridQualitySettings {
    uint32_t reflectionSamples = 4;
    uint32_t historyLimit = 32;
    uint32_t spatialRadius = 1;
    uint32_t glassBoundaryLimit = 16;
    /// @note Zero retains unbounded scene-lighting reflection queries. A finite no-hit requests existing SSR/IBL fallback, not a confirmed environment miss.
    float maxTraceDistance = 0;
    /// @note Per-view reconstruction working set; exceeding it keeps current-frame RAW rendering without allocating oversized histories.
    uint32_t maxHistoryMiB = 512;
    /// @note Shared across views in one resource-manager frame. Zero freezes refresh, not the last valid lighting; uncaptured probes fall back to global IBL.
    uint32_t maxProbeCapturesPerFrame = 1;
    float frameBudgetMs = 1000.0f / 60.0f;
    float asUpdateBudgetMs = 1;
    float traceBudgetMs = 3;
    float reconstructionBudgetMs = 1;
    float probeUpdateBudgetMs = 1;
    bool operator==(const HybridQualitySettings&) const = default;
};

[[nodiscard]] inline bool IsHybridQualityValid(const HybridQualitySettings& value)
{
    if (value.reflectionSamples < 1 || value.reflectionSamples > 64
        || value.historyLimit < 1 || value.historyLimit > 64 || value.spatialRadius > 2
        || value.glassBoundaryLimit < 1 || value.glassBoundaryLimit > 16
        || value.maxHistoryMiB < 1 || value.maxHistoryMiB > 16384 || value.maxProbeCapturesPerFrame > 16
        || !std::isfinite(value.maxTraceDistance) || value.maxTraceDistance < 0) return false;
    for (float target : {value.frameBudgetMs, value.asUpdateBudgetMs, value.traceBudgetMs,
        value.reconstructionBudgetMs, value.probeUpdateBudgetMs})
        if (!std::isfinite(target) || target <= 0 || target > 1000) return false;
    return true;
}

/// @note Presets specify work, not FPS guarantees; selecting one never changes mode, SSR Volume settings or player render scale.
[[nodiscard]] inline HybridQualitySettings MakeHybridQualityPreset(HybridQualityPreset preset)
{
    HybridQualitySettings value;
    if (preset == HybridQualityPreset::LOW) {
        value.reflectionSamples = 1;
        value.historyLimit = 16;
        value.glassBoundaryLimit = 8;
        value.maxTraceDistance = 50;
        value.maxHistoryMiB = 256;
        value.traceBudgetMs = 1;
    } else if (preset == HybridQualityPreset::BALANCED) {
        value.reflectionSamples = 2;
        value.historyLimit = 24;
        value.glassBoundaryLimit = 12;
        value.maxTraceDistance = 100;
        value.traceBudgetMs = 2;
    } else if (preset == HybridQualityPreset::HIGH) {
        value.reflectionSamples = 8;
        value.spatialRadius = 2;
        value.traceBudgetMs = 5;
    }
    return value;
}

[[nodiscard]] inline HybridQualityPreset DetectHybridQualityPreset(const HybridQualitySettings& value)
{
    for (auto preset : {HybridQualityPreset::LOW, HybridQualityPreset::BALANCED, HybridQualityPreset::HIGH})
        if (value == MakeHybridQualityPreset(preset)) return preset;
    return HybridQualityPreset::CUSTOM;
}

} /// @note namespace fbzz::renderer
