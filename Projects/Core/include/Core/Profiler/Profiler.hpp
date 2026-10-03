/// @file    Profiler.hpp
/// @brief   Performance 計測の互換 API と所有 snapshot。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once

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

struct PerformanceSnapshot : ProfileSnapshot {
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

/// @pre 全操作は main thread から呼ぶ。
class Profiler {
public:
    /// @note 即時に token と公開データを失効させる旧契約。新 UI は RequestRecording を使う。
    static void SetEnabled(bool enabled);
    static bool IsEnabled();
    static void RequestRecording(bool recording);
    static void RequestClear();
    static void BeginFrame();
    static void BeginFrame(uint64_t applicationFrameSerial, double frameStartMs);
    static void EndFrame();
    static void BeginSample(const ProfilerMarker& marker);
    static void EndSample();
    static ProfileToken BeginScope(const ProfilerMarker& marker);
    static void EndScope(ProfileToken token);
    static void PushMarker(const ProfilerMarker& marker);
    /// @note join 後の外部経過時間は Self と開始位置を unavailable とする。
    static void PushSample(const ProfilerMarker& marker, double elapsedMs);
    static ProfilerCheckpoint Checkpoint();
    static void Recover(ProfilerCheckpoint checkpoint);
    static const std::vector<ProfileRecord>& GetLastFrameRecords();
    static uint64_t GetLastFrameIndex();
    static const PerformanceSnapshot& GetSnapshot();
};

} /// @note namespace fbzz::profiler
