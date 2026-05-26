// FBZZ Engine
// Time.cpp | fbzz::core
// QueryPerformanceCounter によるフレーム時間計測
// Tick で生の delta と TimeScale 適用後 delta を更新する。
// シングルスレッドのゲームループから呼ばれる前提。
#include "Engine/Core/Time.hpp"

#define NOMINMAX
#include <Windows.h>
#include <algorithm>

namespace fbzz::core {

float    Time::s_rawDeltaTime = 0.0f;
float    Time::s_deltaTime    = 0.0f;
float    Time::s_totalTime    = 0.0f;
float    Time::s_timeScale    = 1.0f;
uint64_t Time::s_frameCount   = 0;
int64_t  Time::s_lastCount    = 0;
int64_t  Time::s_frequency    = 0;
int      Time::s_targetFps    = 0;

void Time::Tick()
{
    // FPS キャップ: 目標フレーム時間になるまで待機してから delta を計測
    // sleep で大半を消費し、最後の 2ms はビジーウェイトで精度を確保する
    if (s_targetFps > 0 && s_frequency > 0) {
        const LONGLONG targetTicks = s_frequency / s_targetFps;
        LARGE_INTEGER  cur;
        QueryPerformanceCounter(&cur);
        const LONGLONG remaining = targetTicks - (cur.QuadPart - s_lastCount);
        if (remaining > 0) {
            const LONGLONG sleepTicks = remaining - s_frequency / 500LL; // 2ms 前まで sleep
            if (sleepTicks > 0)
                Sleep((DWORD)(sleepTicks * 1000LL / s_frequency));
            do { QueryPerformanceCounter(&cur); }
            while ((cur.QuadPart - s_lastCount) < targetTicks);
        }
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (s_frequency == 0)
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        s_frequency  = freq.QuadPart;
        s_lastCount  = now.QuadPart;
        s_rawDeltaTime = 0.0f;
        s_deltaTime    = 0.0f;
        ++s_frameCount;
        return;
    }

    const float raw = static_cast<float>(now.QuadPart - s_lastCount)
                    / static_cast<float>(s_frequency);
    s_lastCount = now.QuadPart;

    // デバッガで止めたとき等の暴走防止: 50ms キャップ
    s_rawDeltaTime = std::min(raw, 0.05f);
    s_deltaTime    = s_rawDeltaTime * s_timeScale;
    s_totalTime   += s_deltaTime;
    ++s_frameCount;
}

void Time::SetTimeScale(float scale)
{
    s_timeScale = std::max(0.0f, scale);
}

void Time::SetTargetFps(int fps)
{
    if (fps <= 0)
        s_targetFps = 0;
    else
        s_targetFps = std::max(fps, 30);
}

} // namespace fbzz::core
