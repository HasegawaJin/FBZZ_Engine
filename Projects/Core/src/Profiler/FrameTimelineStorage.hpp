/// @file    FrameTimelineStorage.hpp
/// @brief   Pure frame-window selection and timestamp joins used by the timeline collector.
/// @author  Hasegawa Jin
/// @date    2026-10-16
#pragma once

#include <cstdint>
#include <atomic>

namespace fbzz::profiler::frame_timeline::detail {

/// @note Overflow retains attempted and dropped counts without addressing beyond the fixed publication array.
[[nodiscard]] inline uint64_t ReserveBounded(std::atomic<uint64_t>& attempted, uint64_t capacity,
    std::atomic<uint64_t>& dropped)
{
    const uint64_t index = attempted.fetch_add(1, std::memory_order_relaxed);
    if (index < capacity) return index;
    dropped.fetch_add(1, std::memory_order_relaxed);
    return UINT64_MAX;
}

enum class FrameRole : uint8_t { UNSELECTED, LEADING, SAMPLE, CLOSING_ANCHOR, FINISHED };

/// @note Selection depends only on declared warmup/sample counts, never observed performance.
class FrameWindow final {
public:
    FrameWindow(uint64_t warmup, uint64_t samples) : m_warmup(warmup), m_samples(samples) {}
    [[nodiscard]] FrameRole Next()
    {
        const uint64_t ordinal = m_seen++;
        if (ordinal < m_warmup) return ordinal + 1 == m_warmup ? FrameRole::LEADING : FrameRole::UNSELECTED;
        if (ordinal - m_warmup < m_samples) return FrameRole::SAMPLE;
        return ordinal - m_warmup == m_samples ? FrameRole::CLOSING_ANCHOR : FrameRole::FINISHED;
    }
    [[nodiscard]] uint64_t Seen() const { return m_seen; }
private:
    uint64_t m_warmup = 0, m_samples = 0, m_seen = 0;
};

struct FrameTimes {
    uint64_t begin = 0, end = 0, nextBegin = 0;
    bool ended = false, closed = false;
    [[nodiscard]] bool CpuAvailable() const { return ended && end >= begin; }
    [[nodiscard]] bool IntervalAvailable() const { return closed && nextBegin >= begin; }
    [[nodiscard]] bool TailAvailable() const { return CpuAvailable() && IntervalAvailable() && nextBegin >= end; }
    [[nodiscard]] uint64_t CpuTicks() const { return CpuAvailable() ? end - begin : 0; }
    [[nodiscard]] uint64_t IntervalTicks() const { return IntervalAvailable() ? nextBegin - begin : 0; }
    [[nodiscard]] uint64_t TailTicks() const { return TailAvailable() ? nextBegin - end : 0; }
};

} /// @note namespace fbzz::profiler::frame_timeline::detail
