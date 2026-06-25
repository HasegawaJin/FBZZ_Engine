// FBZZ Engine
// Time.cpp | fbzz
// QueryPerformanceCounter によるフレーム時間計測
#include "Engine/Core/Time.hpp"

#define NOMINMAX
#include <Windows.h>
#include <algorithm>

namespace fbzz {

float    Time::deltaTime         = 0.0f;
float    Time::unscaledDeltaTime = 0.0f;
float    Time::time              = 0.0f;
float    Time::unscaledTime      = 0.0f;
uint64_t Time::frameCount        = 0;
float    Time::timeScale         = 1.0f;
int      Time::targetFps         = 0;
int64_t  Time::s_lastCount       = 0;
int64_t  Time::s_frequency       = 0;

void Time::Tick()
{
    // FPS キャップ: 目標フレーム時間になるまで待機してから delta を計測
    // sleep で大半を消費し、最後の 2ms はビジーウェイトで精度を確保する
    if (targetFps > 0 && s_frequency > 0) {
        const int   clamped     = std::max(targetFps, 30);
        const int64_t targetTicks = s_frequency / clamped;
        LARGE_INTEGER cur;
        QueryPerformanceCounter(&cur);
        const int64_t remaining = targetTicks - (cur.QuadPart - s_lastCount);
        if (remaining > 0) {
            const int64_t sleepTicks = remaining - s_frequency / 500LL;
            if (sleepTicks > 0)
                Sleep((DWORD)(sleepTicks * 1000LL / s_frequency));
            do { QueryPerformanceCounter(&cur); }
            while ((cur.QuadPart - s_lastCount) < targetTicks);
        }
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (s_frequency == 0) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        s_frequency  = freq.QuadPart;
        s_lastCount  = now.QuadPart;
        ++frameCount;
        return;
    }

    const float raw = static_cast<float>(now.QuadPart - s_lastCount)
                    / static_cast<float>(s_frequency);
    s_lastCount = now.QuadPart;

    // デバッガで止めたとき等の暴走防止: 50ms キャップ
    unscaledDeltaTime  = std::min(raw, 0.05f);
    deltaTime          = unscaledDeltaTime * std::max(timeScale, 0.0f);
    time              += deltaTime;
    unscaledTime      += unscaledDeltaTime;
    ++frameCount;
}

void Time::Reset()
{
    time         = 0.0f;
    unscaledTime = 0.0f;
    frameCount   = 0;
}

void Time::SetTimeScale(float scale)
{
    timeScale = std::max(scale, 0.0f);
}

float Time::GetTimeScale()
{
    return timeScale;
}

void Time::SetTargetFps(int fps)
{
    targetFps = fps <= 0 ? 0 : std::max(fps, 30);
}

int Time::GetTargetFps()
{
    return targetFps;
}

} // namespace fbzz
