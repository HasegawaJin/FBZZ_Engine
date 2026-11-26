/// @file    FrameTimelineStorage.hpp
/// @brief   タイムラインで収集するフレームの選択と、境界時刻からの区間計算。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#pragma once

#include <cstdint>
#include <atomic>

namespace fbzz::profiler::frame_timeline::detail {

/// @note 容量を超えた要求も試行数と欠落数に含め、配列の添字に使えない UINT64_MAX を返す。
[[nodiscard]] inline uint64_t ReserveBounded(std::atomic<uint64_t>& attempted, uint64_t capacity,
    std::atomic<uint64_t>& dropped)
{
    const uint64_t index = attempted.fetch_add(1, std::memory_order_relaxed);
    if (index < capacity) {
        return index;
    }
    dropped.fetch_add(1, std::memory_order_relaxed);
    return UINT64_MAX;
}

enum class FrameRole : uint8_t { UNSELECTED, LEADING, SAMPLE, CLOSING_ANCHOR, FINISHED };

/// @note 計測フレームは指定したウォームアップ数とサンプル数だけで選ぶため、処理の速さに左右されない。
class FrameWindow final {
public:
    FrameWindow(uint64_t warmup, uint64_t samples) : m_warmup(warmup), m_samples(samples) {}
    [[nodiscard]] FrameRole Next()
    {
        const uint64_t frameIndex = m_seen++;
        if (frameIndex < m_warmup) {
            const bool lastWarmupFrame = frameIndex + 1 == m_warmup;
            return lastWarmupFrame ? FrameRole::LEADING : FrameRole::UNSELECTED;
        }

        const uint64_t sampleIndex = frameIndex - m_warmup;
        if (sampleIndex < m_samples) {
            return FrameRole::SAMPLE;
        }
        return sampleIndex == m_samples ? FrameRole::CLOSING_ANCHOR : FrameRole::FINISHED;
    }
    [[nodiscard]] uint64_t Seen() const { return m_seen; }
private:
    uint64_t m_warmup = 0; ///< サンプル開始前のウォームアップフレーム数。
    uint64_t m_samples = 0; ///< 計測結果に含めるフレーム数。
    uint64_t m_seen = 0; ///< これまでに役割を判定したフレームの数。
};

/// @brief フレームの処理時間、開始間隔、終了後の余白を算出する境界時刻。
/// @note フレームの処理終了を記録できなくても、次の開始時刻があれば開始から開始までの間隔を求められる。
struct FrameTimes {
    uint64_t begin = 0;     ///< このフレームの処理開始時刻（QPC ティック）。
    uint64_t end = 0;       ///< このフレームの処理終了時刻（QPC ティック）。ended が true のとき有効。
    uint64_t nextBegin = 0; ///< 次のフレームの開始時刻（QPC ティック）。closed が true のとき有効。
    bool ended = false;     ///< このフレームの処理終了を観測できたとき true。
    bool closed = false;    ///< 次のフレーム開始を観測できたとき true。開始時刻どうしの間隔が確定する。

    [[nodiscard]] bool CpuAvailable() const { return ended && end >= begin; }
    [[nodiscard]] bool IntervalAvailable() const { return closed && nextBegin >= begin; }
    [[nodiscard]] bool TailAvailable() const { return CpuAvailable() && IntervalAvailable() && nextBegin >= end; }
    [[nodiscard]] uint64_t CpuTicks() const { return CpuAvailable() ? end - begin : 0; }
    [[nodiscard]] uint64_t IntervalTicks() const { return IntervalAvailable() ? nextBegin - begin : 0; }
    [[nodiscard]] uint64_t TailTicks() const { return TailAvailable() ? nextBegin - end : 0; }
};

} // namespace fbzz::profiler::frame_timeline::detail
