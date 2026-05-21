// FBZZ Engine
// Time.cpp | fbzz::core
// フレームタイム計測 (QueryPerformanceCounter)
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

void Time::Tick()
{
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

} // namespace fbzz::core
