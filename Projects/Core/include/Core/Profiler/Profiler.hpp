/// @file    Profiler.hpp
/// @brief   Performance 計測の互換 API と所有 snapshot。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once
#include <Core/Api.hpp>

#include <Core/Profiler/ProfilerMarker.hpp>
#include <Core/Profiler/ProfileRecorder.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::profiler {

/// @note 旧 ABI のレイアウトを維持する。文字列は次の EndFrame まで有効。
struct ProfileRecord {
    const char* name = "Unnamed";
    const char* category = "General";
    double elapsedMs = 0.0;
    uint64_t frameIndex = 0;
    uint32_t depth = 0;
    uint32_t color = 0xFF4FA3FF;
};

struct PerformanceDescriptor {
    uint64_t sampleKey = 0;
    std::string name;
    std::string category;
    uint32_t color = 0xFF4FA3FF;
};

struct PerformanceCaptureFilter {
    /// @note category は完全一致、scope は大文字小文字を区別する部分一致。空欄は全対象。
    std::string category;
    std::string scope;
    bool operator==(const PerformanceCaptureFilter&) const = default;
};

struct PerformanceSnapshot : ProfileSnapshot {
    PerformanceCaptureFilter captureFilter;
    std::vector<PerformanceDescriptor> descriptors;
};

/// @note recorder と互換 BeginSample の対応を同時に保存する、所有ポインターを持たない SEH 境界。
struct ProfilerCheckpoint {
    ProfileCheckpoint recorder;
    ProfileCheckpoint compatOverflow;
    uint64_t compatTopScopeId = 0;
    uint64_t compatOverflowScopeId = 0;
    uint32_t compatStackSize = 0;
    uint32_t compatSuppressedDepth = 0;
};

/// @note recorder、recording 要求、descriptor と公開 snapshot はスレッドごとに独立し、収集フィルタだけを共有する。
/// @pre token/checkpoint の開始・終了・復元は同じスレッドで行うこと。
class Profiler {
public:
    /// @note 即時に token と公開データを失効させる旧契約。新 UI は RequestRecording を使う。
    FBZZ_CORE_API static void SetEnabled(bool enabled);
    FBZZ_CORE_API static bool IsEnabled();
    /// @note frame 境界へ未適用の要求も含め、recording の意図を返す。
    FBZZ_CORE_API static bool IsRecordingRequested();
    FBZZ_CORE_API static void RequestRecording(bool recording);
    FBZZ_CORE_API static void RequestClear();
    /// @note 全 Performance スレッドへ次の BeginFrame で適用し、変更時は新しい session を開始する。
    /// @return category が128 byte、scope が256 byteを超える要求は変更せず false。
    FBZZ_CORE_API static bool RequestCaptureFilter(PerformanceCaptureFilter filter);
    FBZZ_CORE_API static PerformanceCaptureFilter GetRequestedCaptureFilter();
    FBZZ_CORE_API static void BeginFrame();
    FBZZ_CORE_API static void BeginFrame(uint64_t applicationFrameSerial, double frameStartMs);
    FBZZ_CORE_API static void EndFrame();
    FBZZ_CORE_API static void BeginSample(const ProfilerMarker& marker);
    FBZZ_CORE_API static void EndSample();
    FBZZ_CORE_API static ProfileToken BeginScope(const ProfilerMarker& marker);
    FBZZ_CORE_API static void EndScope(ProfileToken token);
    FBZZ_CORE_API static void PushMarker(const ProfilerMarker& marker);
    /// @note join 後の外部経過時間は Self と開始位置を unavailable とする。
    FBZZ_CORE_API static void PushSample(const ProfilerMarker& marker, double elapsedMs);
    FBZZ_CORE_API static ProfilerCheckpoint Checkpoint();
    FBZZ_CORE_API static void Recover(ProfilerCheckpoint checkpoint);
    FBZZ_CORE_API static const std::vector<ProfileRecord>& GetLastFrameRecords();
    FBZZ_CORE_API static uint64_t GetLastFrameIndex();
    FBZZ_CORE_API static const PerformanceSnapshot& GetSnapshot();
};

} /// @note namespace fbzz::profiler
