/// @file    GpuProfiler.hpp
/// @brief   完了済み GPU 計測のフレーム・ビュー由来と完全性。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::renderer {

/// @note タグは記録時の値であり、回収時の現在ビューや Plan で上書きしない。
struct GpuProfilerViewMetadata {
    uint64_t applicationFrameSerial = 0;
    uint64_t viewId = 0;
    uint64_t sceneGeneration = 0;
    uint64_t planGeneration = 0;
    uint64_t resourceEpoch = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t outputId = 0;
    uint32_t outputGeneration = 0;
    bool operator==(const GpuProfilerViewMetadata&) const = default;
};

/// @note 非同期表示は許容 age と source serial を併記し、画像への厳密結合は maxFrameAge=0 に限定する。
/// @note 同一フレームの重複記録を区別する token は含まず、Controller の受理判定には用いない。
[[nodiscard]] inline bool IsGpuProfilerViewCompatible(const GpuProfilerViewMetadata& sample,
    const GpuProfilerViewMetadata& current, uint64_t maxFrameAge = 0)
{
    if (sample.applicationFrameSerial > current.applicationFrameSerial
        || current.applicationFrameSerial - sample.applicationFrameSerial > maxFrameAge
        || sample.planGeneration == 0) return false;
    auto expected = current;
    expected.applicationFrameSerial = sample.applicationFrameSerial;
    return sample == expected;
}

/// @note name/gpuMs の先頭配置を維持する。available=false の gpuMs は計測値として消費しない。
struct GpuPassProfile {
    std::string name;
    double gpuMs = 0.0;
    GpuProfilerViewMetadata metadata;
    uint64_t physicalFrameSerial = 0;
    uint64_t deviceEpoch = 0;
    bool available = false;
};

/// @note 最後に完了した物理フレーム一件だけを表し、複数フレームのパスを混ぜない。
/// @note complete は要求したビュー内パス区間の完全性であり、GPU フレーム全体の測定完了ではない。
/// @note 全体・AS 準備・別キュー時間は未計測。パス時間の合計をそれらの代用にしない。
struct GpuProfilerSnapshot {
    bool supported = false;
    bool available = false;
    bool complete = false;
    uint64_t physicalFrameSerial = 0;
    uint64_t deviceEpoch = 0;
    uint32_t recordedPassCount = 0;
    uint32_t droppedPassCount = 0;
    bool totalGpuTimeAvailable = false;
    double totalGpuMs = 0.0;
    bool preparationGpuTimeAvailable = false;
    bool perQueueGpuTimeAvailable = false;
    std::vector<GpuPassProfile> passes;
};

} /// @note namespace fbzz::renderer
