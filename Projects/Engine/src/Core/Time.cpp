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
// 既定値は SceneManager が設定する Phase::Physics の 60Hz と一致させる。
// 実際の値は固定ステップ実行時に FixedScriptSystem が毎回上書きする。
float    Time::fixedDeltaTime    = 1.0f / 60.0f;
float    Time::timeScale         = 1.0f;
float    Time::vfxTimeScale      = 1.0f;
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
    // 2 本の倍率を掛ける。ゲーム側 (timeScale) と VFX 側 (vfxTimeScale) は
    // 互いを知らずに自分の枠だけを書く ─ 同じ変数を奪い合わせない (Time.hpp の WHY)。
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
    // VFX の枠だけ素へ戻す。
    //
    // WHY timeScale は触らないか: あちらはゲームと Editor の設定で、リセットの
    //     たびに 1.0 へ倒すと Inspector のスロー再生が黙って解除される。
    //     こちらはエンジンが握っている一時的な要求なので、シーンをリセットした
    //     時点で «誰も要求していない» が正しい ── 止めたまま Play を抜けた .vfx の
    //     要求が残って «次の Play が最初から遅い» のを防ぐ。
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

} // namespace fbzz
