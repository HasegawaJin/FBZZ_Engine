// FBZZ Engine
// Time.hpp | fbzz
// フレーム時間・経過時間・タイムスケール管理
// Application::Run() 先頭で Time::Tick() を呼ぶ。以降は Time::deltaTime などを直接参照する。
#pragma once
#include <cstdint>

#ifdef _WIN32
#   ifdef fbzz_engine_EXPORTS
#       define FBZZ_ENGINE_API __declspec(dllexport)
#   else
#       define FBZZ_ENGINE_API __declspec(dllimport)
#   endif
#else
#   define FBZZ_ENGINE_API
#endif

namespace fbzz {

struct FBZZ_ENGINE_API Time {
    // ── 読み取り用 (エンジンが毎フレーム更新する) ─────────────────────────
    static float    deltaTime;          // TimeScale 適用済みフレーム秒
    static float    unscaledDeltaTime;  // TimeScale 未適用フレーム秒
    static float    time;               // 累積秒 (TimeScale 適用済み)
    static float    unscaledTime;       // 累積秒 (TimeScale 未適用)
    static uint64_t frameCount;         // 起動からのフレーム数

    // ── 設定 (読み書き可) ────────────────────────────────────────────────
    // 0=停止, 0.5=スローモーション, 1=通常, 2=2倍速
    static float timeScale;
    // 0=無制限, 正値=FPS 上限 (最低 30 FPS にクランプ)
    static int   targetFps;

    // ── エンジン内部用 ───────────────────────────────────────────────────
    static void Tick();    // Application::Run() 先頭で毎フレーム呼ぶ
    static void Reset();   // シーンリセット時に time / frameCount を 0 にする

private:
    static int64_t s_lastCount;
    static int64_t s_frequency;
};

} // namespace fbzz
