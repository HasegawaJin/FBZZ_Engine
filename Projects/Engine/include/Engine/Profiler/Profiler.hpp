/// @file    Profiler.hpp
/// @brief   CPU 計測サンプルをフレーム単位で収集する軽量プロファイラ。
/// @author  Hasegawa Jin
/// @date    2026-06-02
#pragma once

#include <Engine/Profiler/ProfilerMarker.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::profiler {

/// 1 つの計測区間の結果を表すスナップショット。
struct ProfileRecord {
    const char* name       = "Unnamed";
    const char* category   = "General";
    double      elapsedMs  = 0.0;
    uint64_t    frameIndex = 0;
    uint32_t    depth      = 0;
    uint32_t    color      = 0xFF4FA3FF;
};

/// ゲームループから使う CPU プロファイラの静的 API。
/// @note 前フレームのスナップショットを公開し、描画中に収集バッファは書き換えない。
class Profiler {
public:
    /// プロファイラの収集を有効・無効にする。無効時は Begin/End のコストを最小化する。
    static void SetEnabled(bool enabled);
    static bool IsEnabled();

    /// 新しいフレームの収集を開始する。通常はゲームループ先頭で 1 回呼ぶ。
    static void BeginFrame();

    /// 現在フレームの収集を確定し、Viewer が読む前フレームスナップショットへ移す。
    static void EndFrame();

    /// 手動で計測区間を開始する。RAII を使える箇所では ProfileScope を優先する。
    static void BeginSample(const ProfilerMarker& marker);

    /// 直近の BeginSample に対応する計測区間を終了する。
    static void EndSample();

    /// インスタントイベントを記録する。時間幅を持たないため elapsedMs は 0 になる。
    static void PushMarker(const ProfilerMarker& marker);

    /// 既に別の場所で測り終えた区間を、そのままフレームへ積む。
    /// @note s_stack はメインスレッド専用の static のため、ワーカースレッドは std::chrono で測り、
    ///       join 後にメインスレッドからこれで積む。
    /// @param elapsedMs 呼び出し側が測った所要時間。
    static void PushSample(const ProfilerMarker& marker, double elapsedMs);

    /// Viewer やログ出力用に、確定済みフレームの計測結果を参照する。
    static const std::vector<ProfileRecord>& GetLastFrameRecords();

    /// 直近で確定したフレーム番号を返す。
    static uint64_t GetLastFrameIndex();

private:
    using Clock = std::chrono::steady_clock;

    struct ActiveSample {
        ProfilerMarker    marker;
        Clock::time_point startTime;
        uint32_t          depth = 0;
    };

    static std::vector<ActiveSample>  s_stack;
    static std::vector<ProfileRecord> s_currentFrameRecords;
    static std::vector<ProfileRecord> s_lastFrameRecords;
    static uint64_t                   s_currentFrameIndex;
    static uint64_t                   s_lastFrameIndex;
    static bool                       s_enabled;
};

} // namespace fbzz::profiler
