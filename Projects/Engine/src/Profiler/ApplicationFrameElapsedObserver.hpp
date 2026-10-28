/// @file    ApplicationFrameElapsedObserver.hpp
/// @brief   Main-thread-only passive frame boundary values and deterministic accumulation.
/// @author  Hasegawa Jin
/// @date    2026-10-13
#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace fbzz::profiler::detail {

enum class ApplicationFrameElapsedBoundary : uint8_t { NORMAL, WINDOW_CLOSE, REPLACED_BEGIN, INVALID_CLOCK };

struct ApplicationFrameElapsedModeWitness {
    bool nativeRecording = false;
    bool nativeRecordingRequested = false;
    bool scriptRecording = false;
    uint64_t nativeSession = 0;
    uint64_t scriptSession = 0;
    uint64_t runtimeEpoch = 0;
    uint64_t dllGeneration = 0;
    bool operator==(const ApplicationFrameElapsedModeWitness&) const = default;
    [[nodiscard]] bool IsOff() const { return !nativeRecording && !nativeRecordingRequested && !scriptRecording; }
};

struct ApplicationFrameElapsedSnapshot {
    uint64_t observerSessionId = 0;
    uint64_t applicationFrameSerial = 0;
    uint64_t publicationOrdinal = 0;
    ApplicationFrameElapsedModeWitness beginMode;
    ApplicationFrameElapsedModeWitness endMode;
    ApplicationFrameElapsedBoundary boundary = ApplicationFrameElapsedBoundary::NORMAL;
    double cpuFrameElapsedMs = 0.0;
    double wallFrameIntervalMs = 0.0;
    bool available = false;
    bool complete = false;
    bool endModeAvailable = false;
    bool cpuFrameElapsedAvailable = false;
    bool wallFrameIntervalAvailable = false;
};
static_assert(std::is_trivially_copyable_v<ApplicationFrameElapsedSnapshot>);
static_assert(std::is_standard_layout_v<ApplicationFrameElapsedSnapshot>);

/// @note Pure timestamp inputs are the production accumulator contract; this type owns no clock or allocation.
class ApplicationFrameElapsedObserver {
public:
    /// @return Reserved next-boundary session, or zero on session exhaustion.
    uint64_t Arm()
    {
        if (m_nextSession == (std::numeric_limits<uint64_t>::max)()) return 0;
        m_pendingSession = ++m_nextSession;
        m_stopPending = false;
        return m_pendingSession;
    }
    void RequestStop() { m_pendingSession = 0; m_stopPending = true; }
    [[nodiscard]] bool WillObserveNextBegin() const { return m_pendingSession != 0 || (m_active && !m_stopPending); }
    [[nodiscard]] bool NeedsEndClock() const { return m_open; }
    [[nodiscard]] ApplicationFrameElapsedSnapshot Read() const { return m_latest; }
    void Begin(uint64_t serial, double startMs, ApplicationFrameElapsedModeWitness mode)
    {
        if (m_stopPending) {
            m_active = false; m_open = false; m_havePrevious = false; m_stopPending = false;
        }
        if (m_pendingSession != 0) {
            m_session = m_pendingSession; m_pendingSession = 0;
            m_active = true; m_open = false; m_havePrevious = false; m_ordinal = 0; m_latest = {};
        }
        if (!m_active) return;
        if (m_open) {
            m_current.boundary = ApplicationFrameElapsedBoundary::REPLACED_BEGIN;
            Publish(); m_havePrevious = false;
        }
        m_current = {};
        m_current.observerSessionId = m_session;
        m_current.applicationFrameSerial = serial;
        m_current.beginMode = mode;
        m_startMs = startMs;
        if (!std::isfinite(startMs)) m_current.boundary = ApplicationFrameElapsedBoundary::INVALID_CLOCK;
        if (m_havePrevious && serial > m_previousSerial) {
            const double interval = startMs - m_previousStartMs;
            m_current.wallFrameIntervalAvailable = std::isfinite(interval) && interval >= 0.0;
            if (m_current.wallFrameIntervalAvailable) m_current.wallFrameIntervalMs = interval;
            else m_current.boundary = ApplicationFrameElapsedBoundary::INVALID_CLOCK;
        }
        m_havePrevious = std::isfinite(startMs);
        m_previousStartMs = startMs; m_previousSerial = serial;
        m_open = true;
    }
    void MarkAborted() { if (m_open) m_current.boundary = ApplicationFrameElapsedBoundary::WINDOW_CLOSE; }
    void End(double endMs, ApplicationFrameElapsedModeWitness mode)
    {
        if (!m_open) return;
        m_current.endMode = mode; m_current.endModeAvailable = true;
        const double elapsed = endMs - m_startMs;
        m_current.cpuFrameElapsedAvailable = std::isfinite(m_startMs) && std::isfinite(endMs)
            && std::isfinite(elapsed) && elapsed >= 0.0;
        if (m_current.cpuFrameElapsedAvailable) m_current.cpuFrameElapsedMs = elapsed;
        else if (m_current.boundary == ApplicationFrameElapsedBoundary::NORMAL)
            m_current.boundary = ApplicationFrameElapsedBoundary::INVALID_CLOCK;
        Publish();
        if (!m_latest.complete) m_havePrevious = false;
    }
private:
    void Publish()
    {
        m_current.available = true;
        m_current.complete = m_current.boundary == ApplicationFrameElapsedBoundary::NORMAL
            && m_current.cpuFrameElapsedAvailable && m_current.endModeAvailable;
        m_current.publicationOrdinal = ++m_ordinal;
        m_latest = m_current;
        m_open = false;
    }
    ApplicationFrameElapsedSnapshot m_current;
    ApplicationFrameElapsedSnapshot m_latest;
    uint64_t m_nextSession = 0, m_pendingSession = 0, m_session = 0, m_ordinal = 0, m_previousSerial = 0;
    double m_startMs = 0.0, m_previousStartMs = 0.0;
    bool m_active = false, m_open = false, m_stopPending = false, m_havePrevious = false;
};

/// @pre All bridge calls are on the application main thread; state remains inside FBZZEngine.dll.
[[nodiscard]] uint64_t ArmApplicationFrameElapsedObserver();
void RequestStopApplicationFrameElapsedObserver();
[[nodiscard]] ApplicationFrameElapsedSnapshot ReadApplicationFrameElapsedObserver();
void MarkApplicationFrameElapsedAborted();

} /// @note namespace fbzz::profiler::detail
