// FBZZ Engine
// Tests/Time/main.cpp
// Time モジュール単体テスト
// fbzz::Time の deltaTime / frameCount / timeScale / Reset を検証する。
// 実時間に依存する部分は Sleep で保証する (テスト専用。ゲームロジックでは Sleep 禁止)。
#include <cstdio>
#include <cmath>
#include <thread>
#include <chrono>

#include <Engine/Core/Time.hpp>

#include "../TestHelper.hpp"

using namespace fbzz;

// ─── 初期状態・Reset ──────────────────────────────────────────────────────────

static void TestTime_ResetInitialState()
{
    std::printf("\n=== Time: Reset / Initial State ===\n");

    Time::Reset();

    checkF(Time::time == 0.0f,
           "Time: Reset clears accumulated time", Time::time, "== 0");
    checkF(Time::unscaledTime == 0.0f,
           "Time: Reset clears unscaled time", Time::unscaledTime, "== 0");
    check(Time::frameCount == 0,
          "Time: Reset clears frameCount");
}

// ─── Tick でフレームカウントが増える ──────────────────────────────────────────

static void TestTime_TickFrameCount()
{
    std::printf("\n=== Time: Tick increments frameCount ===\n");

    Time::Reset();
    Time::timeScale = 1.0f;

    // 1 フレーム目 (QPC 初期化のため deltaTime は 0 になる場合がある)
    Time::Tick();
    check(Time::frameCount == 1, "Time: frameCount == 1 after first Tick");

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Time::Tick();
    check(Time::frameCount == 2, "Time: frameCount == 2 after second Tick");

    checkF(Time::unscaledDeltaTime > 0.0f,
           "Time: unscaledDeltaTime > 0 after sleep",
           Time::unscaledDeltaTime, "> 0");
}

// ─── deltaTime = unscaledDeltaTime * timeScale ────────────────────────────────

static void TestTime_TimeScale()
{
    std::printf("\n=== Time: timeScale ===\n");

    // timeScale = 0 → deltaTime == 0、unscaledDeltaTime は正
    {
        Time::Reset();
        Time::timeScale = 0.0f;
        Time::Tick();

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Time::Tick();

        checkF(Time::deltaTime == 0.0f,
               "Time: timeScale=0 makes deltaTime == 0",
               Time::deltaTime, "== 0");
        checkF(Time::unscaledDeltaTime > 0.0f,
               "Time: timeScale=0 does not affect unscaledDeltaTime",
               Time::unscaledDeltaTime, "> 0");
    }

    // timeScale = 2 → deltaTime == unscaledDeltaTime * 2
    {
        Time::Reset();
        Time::timeScale = 2.0f;
        Time::Tick();

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Time::Tick();

        const float ratio = (Time::unscaledDeltaTime > 0.0f)
                            ? Time::deltaTime / Time::unscaledDeltaTime
                            : 0.0f;
        checkF(std::abs(ratio - 2.0f) < 0.05f,
               "Time: timeScale=2 doubles deltaTime",
               ratio, "~= 2.0");
    }

    // timeScale = 0.5 → deltaTime ≈ unscaledDeltaTime / 2
    {
        Time::Reset();
        Time::timeScale = 0.5f;
        Time::Tick();

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Time::Tick();

        const float ratio = (Time::unscaledDeltaTime > 0.0f)
                            ? Time::deltaTime / Time::unscaledDeltaTime
                            : 0.0f;
        checkF(std::abs(ratio - 0.5f) < 0.05f,
               "Time: timeScale=0.5 halves deltaTime",
               ratio, "~= 0.5");
    }
}

// ─── time / unscaledTime の累積 ───────────────────────────────────────────────

static void TestTime_AccumulatedTime()
{
    std::printf("\n=== Time: Accumulated time ===\n");

    Time::Reset();
    Time::timeScale = 1.0f;

    for (int i = 0; i < 5; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Time::Tick();
    }

    checkF(Time::time > 0.0f,
           "Time: accumulated time > 0 after 5 Ticks",
           Time::time, "> 0");
    checkF(Time::unscaledTime > 0.0f,
           "Time: accumulated unscaledTime > 0 after 5 Ticks",
           Time::unscaledTime, "> 0");
    checkF(std::abs(Time::time - Time::unscaledTime) < 0.001f,
           "Time: time ~= unscaledTime when timeScale == 1",
           Time::time - Time::unscaledTime, "~= 0");
    check(Time::frameCount == 5, "Time: frameCount == 5 after 5 Ticks");

    // timeScale = 2 で累積するとスケール有りの方が大きくなる
    const float prevTime = Time::time;
    const float prevUnscaled = Time::unscaledTime;

    Time::timeScale = 2.0f;
    for (int i = 0; i < 3; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Time::Tick();
    }

    const float addedTime    = Time::time - prevTime;
    const float addedUnscaled = Time::unscaledTime - prevUnscaled;
    checkF(addedTime > addedUnscaled * 1.8f,
           "Time: timeScale=2 accumulates time faster than unscaledTime",
           addedTime / (addedUnscaled + 1e-6f), "ratio > 1.8");
}

// ─── targetFps クランプ ────────────────────────────────────────────────────────

static void TestTime_TargetFps()
{
    std::printf("\n=== Time: targetFps clamp ===\n");

    // targetFps は最低 30 FPS にクランプされる仕様
    Time::targetFps = 10; // 30 未満 → 30 にクランプ
    // Tick の内部でクランプが起きるため、設定後に 1 フレーム進める
    Time::Reset();
    Time::Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Time::Tick();

    // クランプの直接観測は難しいが、クラッシュしないことと
    // deltaTime が異常値 (>1s) にならないことを確認する
    checkF(Time::unscaledDeltaTime < 1.0f,
           "Time: targetFps=10 does not produce deltaTime > 1s",
           Time::unscaledDeltaTime, "< 1.0");

    // 0 = 無制限
    Time::targetFps = 0;
    Time::Tick();
    check(true, "Time: targetFps=0 (unlimited) does not crash");

    // 元に戻す
    Time::targetFps = 0;
    Time::timeScale = 1.0f;
}

// ─── エントリポイント ─────────────────────────────────────────────────────────

int main()
{
    std::printf("FBZZ Time Tests\n");
    std::printf("===============\n");

    TestTime_ResetInitialState();
    TestTime_TickFrameCount();
    TestTime_TimeScale();
    TestTime_AccumulatedTime();
    TestTime_TargetFps();

    std::printf("\n===============\n");
    std::printf("Results: %d passed, %d failed\n", g_passed, g_failed);

    return g_failed == 0 ? 0 : 1;
}
