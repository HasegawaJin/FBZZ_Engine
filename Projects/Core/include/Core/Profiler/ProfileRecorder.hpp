/// @file    ProfileRecorder.hpp
/// @brief   対象に依存しない有界な CPU 区間収集と障害復旧。
/// @author  Hasegawa Jin
/// @date    2026-10-03
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::profiler {

enum class ProfileSampleStatus : uint8_t { COMPLETE, FAULTED, ABORTED };
enum class ProfileSampleKind : uint8_t { TIMED_SCOPE, EXTERNAL_DURATION, INSTANT };

struct ProfileSample {
    uint64_t sampleId = 0;
    uint64_t parentSampleId = 0;
    uint64_t sampleKey = 0;
    double startOffsetMs = 0.0;
    double inclusiveMs = 0.0;
    double selfMs = 0.0;
    uint32_t depth = 0;
    uint32_t completionOrder = 0;
    ProfileSampleStatus status = ProfileSampleStatus::COMPLETE;
    ProfileSampleKind sampleKind = ProfileSampleKind::TIMED_SCOPE;
    bool startAvailable = true;
    bool selfAvailable = true;
};

/// @note 所有ポインターを持たず、SEH 境界を値として越えられる。
struct ProfileToken {
    uint64_t recorderId = 0;
    uint64_t generation = 0;
    uint64_t frame = 0;
    uint64_t scopeId = 0;
    uint64_t parentScopeId = 0;
    uint32_t depth = 0;
    bool suppressed = false;
};

struct ProfileCheckpoint {
    uint64_t recorderId = 0;
    uint64_t generation = 0;
    uint64_t frame = 0;
    uint64_t topScopeId = 0;
    uint64_t suppressionTopScopeId = 0;
    uint32_t stackSize = 0;
    uint32_t suppressionDepth = 0;
};

struct ProfileSnapshot {
    bool recording = false;
    bool available = false;
    bool complete = true;
    bool wallFrameIntervalAvailable = false;
    uint64_t captureSessionId = 1;
    uint64_t applicationFrameSerial = 0;
    uint64_t frameIndex = 0;
    uint64_t unframedInvocationCount = 0;
    uint32_t recordedSampleCount = 0;
    uint32_t droppedSampleCount = 0;
    double wallFrameIntervalMs = 0.0;
    double cpuFrameElapsedMs = 0.0;
    double scopeRootSumMs = 0.0;
    std::vector<ProfileSample> samples;
};

/// @pre 全操作はホストの main thread から呼ぶ。
/// @note 時刻は単調時計のミリ秒。注入時計はテストにも同じ契約を要求する。
class ProfileRecorder {
public:
    using ClockFn = double (*)();
    static constexpr size_t MAX_SAMPLES = 8192;
    static constexpr size_t MAX_DEPTH = 128;
    explicit ProfileRecorder(bool recording = true, ClockFn clock = nullptr);
    ProfileRecorder(const ProfileRecorder&) = delete;
    ProfileRecorder& operator=(const ProfileRecorder&) = delete;
    static double NowMs();
    double ReadClock() const;
    bool IsRecording() const { return m_recording; }
    bool IsFrameOpen() const { return m_frameOpen; }
    void RequestRecording(bool recording);
    void RequestClear();
    /// @note 旧 API 専用の即時失効。新しい UI は RequestRecording を使う。
    void SetRecordingImmediate(bool recording);
    void BeginFrame(uint64_t applicationFrameSerial, double frameStartMs);
    void EndFrame();
    ProfileToken Begin(uint64_t sampleKey);
    void End(ProfileToken token, ProfileSampleStatus status = ProfileSampleStatus::COMPLETE);
    void Push(uint64_t sampleKey, double durationMs, ProfileSampleKind kind);
    /// @note 抑制中の生存境界は最大 MAX_SAMPLES 件。超過は無効 checkpoint を返し、外側の既存境界は保持する。
    ProfileCheckpoint Checkpoint() const;
    bool Recover(ProfileCheckpoint checkpoint);
    const ProfileSnapshot& GetSnapshot() const { return m_snapshot; }
private:
    struct ActiveScope {
        ProfileToken token;
        double startMs = 0.0;
        double childMs = 0.0;
        size_t sampleIndex = MAX_SAMPLES;
        bool selfAvailable = true;
    };
    struct SuppressionAnchor {
        uint64_t scopeId = 0;
        uint32_t depth = 0;
    };
    bool Matches(uint64_t recorderId, uint64_t generation, uint64_t frame) const;
    void MarkIncomplete();
    void Reset();
    ClockFn m_clock = nullptr;
    uint64_t m_recorderId = 0;
    uint64_t m_generation = 1;
    uint64_t m_frameIndex = 0;
    uint64_t m_nextScopeId = 0;
    uint64_t m_nextSampleId = 0;
    uint64_t m_captureSessionId = 1;
    uint64_t m_unframedInvocationCount = 0;
    uint64_t m_suppressionTopScopeId = 0;
    uint32_t m_suppressionDepth = 0;
    uint32_t m_completionOrder = 0;
    double m_frameStartMs = 0.0;
    double m_previousStartMs = 0.0;
    bool m_havePreviousStart = false;
    bool m_recording = true;
    bool m_frameOpen = false;
    bool m_pendingRecording = true;
    bool m_hasPendingRecording = false;
    bool m_pendingClear = false;
    std::vector<ActiveScope> m_stack;
    mutable std::vector<SuppressionAnchor> m_suppressionAnchors;
    ProfileSnapshot m_current;
    ProfileSnapshot m_snapshot;
};

} /// @note namespace fbzz::profiler
