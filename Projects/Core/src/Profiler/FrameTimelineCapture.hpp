/// @file    FrameTimelineCapture.hpp
/// @brief   収集とファイル出力が共有するタイムラインのデータ。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Core/Profiler/FrameTimeline.hpp>
#include "FrameTimelineStorage.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace fbzz::profiler::frame_timeline::detail {

inline constexpr uint64_t INVALID_INDEX = UINT64_MAX;

inline constexpr size_t CAPTURE_ANCHOR_COUNT = 2;

inline constexpr unsigned DEFERRED_SLOT_BITS = 6;
inline constexpr size_t MAX_PENDING_FINE_SPANS = size_t{1} << DEFERRED_SLOT_BITS;
inline constexpr uint64_t FINE_SPANS_PER_SECOND = 200000;

enum class Edge : uint8_t { BEGIN, END, INSTANT };

struct CpuCounters {
    uint64_t kernel = 0, user = 0, cycles = 0;
    bool cpuAvailable = false, cyclesAvailable = false;
};
struct EventRow {
    Context context{};
    uint64_t qpc = 0, scope = 0, value = 0, cpu = INVALID_INDEX;
    uint32_t osThreadId = 0;
    Event event = Event::MAIN_FRAME;
    Edge edge = Edge::INSTANT;
    bool threadCpu = false;
    std::atomic<bool> ended{false};
};
struct FrameRow {
    Context context{};
    detail::FrameTimes times;
    double profileBeginMs = 0.0, profileEndMs = 0.0;
    uint32_t osThreadId = 0;
    detail::FrameRole role = detail::FrameRole::UNSELECTED;
};
struct Capture {
    Capture(uint64_t session, size_t warmupCount, size_t requestedSamples, size_t maxEvents)
        : observerSession(session),
          warmup(warmupCount),
          samples(requestedSamples),
          eventCapacity(maxEvents),
          cpuCapacity((requestedSamples + CAPTURE_ANCHOR_COUNT) * 4 + 64),
          window(warmupCount, requestedSamples),
          events(std::make_unique<EventRow[]>(maxEvents)),
          cpu(std::make_unique<CpuCounters[]>(cpuCapacity)),
          frames(std::make_unique<FrameRow[]>(requestedSamples + CAPTURE_ANCHOR_COUNT))
    {
    }
    uint64_t observerSession = 0, warmup = 0, samples = 0, frequency = 0;
    size_t eventCapacity = 0, cpuCapacity = 0;
    detail::FrameWindow window;
    std::unique_ptr<EventRow[]> events;
    std::unique_ptr<CpuCounters[]> cpu;
    std::unique_ptr<FrameRow[]> frames;
    std::atomic<uint64_t> eventCount{0}, cpuCount{0}, droppedEvents{0}, droppedCpu{0};
    std::atomic<uint64_t> activeWriters{0}, activeScopes{0}, wallQueries{0}, cpuQueries{0}, invalidBoundaries{0};
    std::array<std::atomic<uint64_t>, static_cast<size_t>(Event::COUNT)> filteredFineScopes{}, filteredFineTicks{};
    std::atomic<uint64_t> fineSpanFallbacks{0}, fineTicketSequence{0};
    std::atomic<bool> closing{false}, cancelled{false};
    size_t frameCount = 0, sampleCount = 0;
    bool closingAnchor = false, exported = false;
};

/// @note Only these high-frequency scopes omit spans shorter than 5 microseconds.
inline bool IsFineSpan(Event event)
{
    return event == Event::EXECUTION_MUTEX || event == Event::THREAD_MUTEX
        || event == Event::WAIT_SLOT || event == Event::PACKET_DESTROY;
}

inline uint64_t FineThresholdTicks(const Capture& capture)
{
    return capture.frequency / FINE_SPANS_PER_SECOND + (capture.frequency % FINE_SPANS_PER_SECOND != 0);
}

/// @pre 新規イベントの受付を止め、残っている書き込みと計測区間を完了していること。
bool PublishCaptureFiles(const Capture& capture, const std::string& outputFile);

} /// @note namespace fbzz::profiler::frame_timeline::detail
