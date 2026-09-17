/// @file    Time.cpp
/// @brief   QueryPerformanceCounter によるフレーム時間計測。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include "Engine/Core/Time.hpp"

#include <Windows.h>
#include <algorithm>

namespace fbzz {

float    Time::deltaTime         = 0.0f;
float    Time::unscaledDeltaTime = 0.0f;
float    Time::time              = 0.0f;
float    Time::unscaledTime      = 0.0f;
uint64_t Time::frameCount        = 0;
/// @note 既定値は SceneManager が設定する Phase::Physics の 60Hz と一致させる。実値は FixedScriptSystem が毎回上書きする。
float    Time::fixedDeltaTime    = 1.0f / 60.0f;
float    Time::timeScale         = 1.0f;
float    Time::vfxTimeScale      = 1.0f;
int      Time::targetFps         = 0;
int64_t  Time::s_lastCount       = 0;
int64_t  Time::s_frequency       = 0;
float    Time::s_lockstepDelta   = 0.0f;

void Time::Tick()
{
    /// @note FPS キャップは sleep で大半を消費し、最後の 2ms はビジーウェイトで精度を確保する。ロックステップ中は待たない。
    if (targetFps > 0 && s_frequency > 0 && s_lockstepDelta <= 0.0f) {
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
    /// @note ロックステップ中も基準時刻は進める。解除した最初のフレームに «止めていた間» の実時間が載らないように。
    s_lastCount = now.QuadPart;

    /// @note 50ms キャップはデバッガで止めたとき等の暴走防止。
    unscaledDeltaTime  = s_lockstepDelta > 0.0f ? s_lockstepDelta : std::min(raw, 0.05f);
    deltaTime          = unscaledDeltaTime * GetEffectiveTimeScale();
    time              += deltaTime;
    unscaledTime      += unscaledDeltaTime;
    ++frameCount;
}

void Time::Reset()
{
    time         = 0.0f;
    unscaledTime = 0.0f;
    frameCount   = 0;
    /// @note VFX の枠だけ素へ戻す。止めたまま Play を抜けた .vfx の要求が残ると «次の Play が最初から遅い» になる。
    vfxTimeScale = 1.0f;
}

void Time::SetTimeScale(float scale)
{
    timeScale = std::max(scale, 0.0f);
}

float Time::GetTimeScale()
{
    return timeScale;
}

float Time::GetEffectiveTimeScale()
{
    return std::max(timeScale, 0.0f) * std::max(vfxTimeScale, 0.0f);
}

void Time::SetTargetFps(int fps)
{
    targetFps = fps <= 0 ? 0 : std::max(fps, 30);
}

int Time::GetTargetFps()
{
    return targetFps;
}

void Time::SetLockstepDelta(float seconds)
{
    s_lockstepDelta = seconds > 0.0f ? std::min(seconds, 0.25f) : 0.0f;
}

float Time::GetLockstepDelta()
{
    return s_lockstepDelta;
}

} // namespace fbzz
