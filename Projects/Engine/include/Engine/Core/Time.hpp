// FBZZ Engine
// Time.hpp | fbzz::core
// フレーム時間・経過時間・タイムスケール管理
// ゲームループ先頭で Tick し、以降の System は DeltaTime を参照する。
// UI や演出用に UnscaledDeltaTime も保持する。
#pragma once
#include <cstdint>

namespace fbzz::core {

class Time {
public:
    // Application::Run() 先頭で毎フレーム呼ぶ。QPC で delta を自己計測する
    static void Tick();

    // TimeScale を考慮した DeltaTime
    static float    DeltaTime()         { return s_deltaTime; }
    // TimeScale の影響を受けない DeltaTime (UI・エフェクト等)
    static float    UnscaledDeltaTime() { return s_rawDeltaTime; }
    // 起動からの累計秒 (TimeScale 適用済み)
    static float    TotalTime()         { return s_totalTime; }
    // 起動からの総フレーム数
    static uint64_t FrameCount()        { return s_frameCount; }

    static float TimeScale()            { return s_timeScale; }
    // 0.0f で停止, 0.5f でスローモーション, 1.0f が通常
    static void  SetTimeScale(float scale);

    static int  TargetFps()            { return s_targetFps; }
    // 0 = 無制限, 正値 = フレームレート上限 (最低 30 FPS にクランプ)
    static void SetTargetFps(int fps);

private:
    static float    s_rawDeltaTime;
    static float    s_deltaTime;
    static float    s_totalTime;
    static float    s_timeScale;
    static uint64_t s_frameCount;
    static int64_t  s_lastCount;
    static int64_t  s_frequency;
    static int      s_targetFps;
};

} // namespace fbzz::core
