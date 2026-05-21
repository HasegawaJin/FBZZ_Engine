// FBZZ Engine
// Time.hpp | fbzz::core
// フレームタイム・経過時間の管理
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

private:
    static float    s_rawDeltaTime;
    static float    s_deltaTime;
    static float    s_totalTime;
    static float    s_timeScale;
    static uint64_t s_frameCount;
    static int64_t  s_lastCount;
    static int64_t  s_frequency;
};

} // namespace fbzz::core
